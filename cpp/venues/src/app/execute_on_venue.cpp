#include "app/execute_on_venue.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

#include "cex/common/env.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

namespace cex::venues::app {

namespace {

using fob::common::v1::Instrument;
using fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
using fob::common::v1::TIF_GTC;
using fob::common::v1::TIF_IOC;
using fob::common::v1::TIF_UNSPECIFIED;
using fob::execution::v1::EXEC_STRATEGY_LIMIT;
using fob::execution::v1::EXEC_STRATEGY_MARKET;
using fob::execution::v1::EXEC_STRATEGY_UNSPECIFIED;
using fob::execution::v1::ExecutionIntent;
using fob::execution::v1::ExecutionReport;

bool has_limit_price(const ExecutionIntent& intent) {
  return intent.has_limit_price() && intent.limit_price().units() != 0;
}

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
    s.erase(s.begin());
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
    s.pop_back();
  }
  return s;
}

std::string normalize_venue_symbol(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '/'), s.end());
  s.erase(std::remove_if(s.begin(), s.end(), [](const unsigned char c) {
            return std::isspace(c) != 0;
          }),
          s.end());
  return s;
}

Instrument normalize_instrument(const Instrument& instrument) {
  if (!instrument.symbol().empty()) return instrument;

  Instrument out = instrument;
  if (!instrument.base().empty() && !instrument.quote().empty()) {
    out.set_symbol(instrument.base() + "/" + instrument.quote());
  }
  return out;
}

std::string canonical_symbol(const Instrument& instrument) {
  if (!instrument.symbol().empty()) return instrument.symbol();
  if (!instrument.base().empty() && !instrument.quote().empty()) {
    return instrument.base() + "/" + instrument.quote();
  }
  return "";
}

std::string default_venue_symbol(const Instrument& instrument) {
  if (!instrument.base().empty() && !instrument.quote().empty()) {
    return instrument.base() + instrument.quote();
  }
  return normalize_venue_symbol(canonical_symbol(instrument));
}

std::unordered_map<std::string, std::string> parse_symbol_map(const std::string& text) {
  std::unordered_map<std::string, std::string> out;
  std::string cur;

  auto flush = [&]() {
    const std::string item = trim(cur);
    cur.clear();
    if (item.empty()) return;

    const std::size_t eq = item.find('=');
    if (eq == std::string::npos) return;

    const std::string lhs = trim(item.substr(0, eq));
    const std::string rhs = normalize_venue_symbol(trim(item.substr(eq + 1)));
    if (lhs.empty() || rhs.empty()) return;
    out[lhs] = rhs;
  };

  for (const char c : text) {
    if (c == ',' || c == ';' || c == '\n') {
      flush();
      continue;
    }
    cur.push_back(c);
  }
  flush();
  return out;
}

domain::VenueOrderResult reject(const std::string& code, const std::string& message) {
  domain::VenueOrderResult out;
  out.accepted = false;
  out.status = EXECUTION_REPORT_STATUS_REJECTED;
  out.error_code = code;
  out.error_message = message;
  return out;
}

ExecutionReport make_execution_report(
    const ExecutionIntent& intent,
    const std::string& venue_id,
    const domain::VenueOrderResult& result) {
  ExecutionReport rep;

  auto* meta = rep.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("venues");
  meta->set_correlation_id(
      intent.meta().correlation_id().empty()
          ? intent.intent_id()
          : intent.meta().correlation_id());
  meta->set_partition_key(intent.intent_id());

  rep.set_report_id(cex::common::uuid_v4());
  rep.set_intent_id(intent.intent_id());
  rep.set_venue(venue_id.empty() ? intent.venue() : venue_id);
  *rep.mutable_instrument() = intent.instrument();
  rep.set_venue_symbol(intent.venue_symbol());
  rep.set_venue_order_id(result.venue_order_id);
  rep.set_client_order_id(intent.client_order_id());
  rep.set_status(result.status);
  *rep.mutable_filled_qty() = result.filled_qty.to_proto();
  *rep.mutable_remaining_qty() = result.remaining_qty.to_proto();
  *rep.mutable_average_price() = result.average_price.to_proto();

  if (!result.error_code.empty() || !result.error_message.empty()) {
    rep.mutable_error()->set_code(result.error_code);
    rep.mutable_error()->set_message(result.error_message);
  }

  // F12-BACKTEST-2 — surface VenueSim RawExecutionEvent enrichment on the
  // ExecutionReport. Production adapters that leave these fields at zero
  // simply produce no-op writes here.
  rep.set_slippage_bps(result.slippage_bps);
  if (result.fee.units != 0 || !result.fee_currency.empty()) {
    auto* fee = rep.mutable_fee_total();
    fee->set_fee_type("taker");
    if (result.fee_rate.units != 0) {
      *fee->mutable_rate() = result.fee_rate.to_proto();
    }
    auto* cost = fee->mutable_cost();
    cost->set_currency(result.fee_currency);
    *cost->mutable_amount() = result.fee.to_proto();
  }

  return rep;
}

}  // namespace

ExecuteOnVenue::ExecuteOnVenue()
    : symbol_map_(parse_symbol_map(cex::common::Env::get_string("VENUE_SYMBOL_MAP", ""))) {}

void ExecuteOnVenue::SetVenueSymbolOverride(const std::string& venue_id,
                                            const std::string& venue_symbol) {
  if (venue_id.empty()) return;
  const std::string normalized = normalize_venue_symbol(venue_symbol);
  std::lock_guard<std::mutex> lock(mu_);
  if (normalized.empty()) {
    runtime_symbol_map_.erase(venue_id);
    return;
  }
  runtime_symbol_map_[venue_id] = normalized;
}

ExecutionReport ExecuteOnVenue::Run(const ExecutionIntent& intent,
                                    domain::VenueAdapter* adapter) const {
  const ExecutionIntent child = BuildChildIntent(intent, adapter);
  if (adapter == nullptr) {
    return make_execution_report(
        child,
        child.venue(),
        reject("VENUE_NOT_FOUND", "No venue adapter registered for venue=" + child.venue()));
  }

  const auto hb = adapter->Heartbeat();
  if (hb.status == domain::VenueConnectionStatus::kDisconnected &&
      !adapter->Reconnect()) {
    return make_execution_report(
        child,
        adapter->VenueId(),
        reject("RECONNECT_FAILED", "Venue adapter reconnect failed"));
  }

  return make_execution_report(child, adapter->VenueId(), adapter->SendOrder(child));
}

ExecutionIntent ExecuteOnVenue::BuildChildIntent(const ExecutionIntent& intent,
                                                 domain::VenueAdapter* adapter) const {
  ExecutionIntent out = intent;
  if (adapter != nullptr && !adapter->VenueId().empty()) {
    out.set_venue(adapter->VenueId());
  }

  *out.mutable_instrument() = normalize_instrument(intent.instrument());

  const std::string venue_symbol = ResolveVenueSymbol(out, out.venue());
  if (!venue_symbol.empty()) out.set_venue_symbol(venue_symbol);

  if (out.client_order_id().empty() && !out.intent_id().empty()) {
    out.set_client_order_id(out.intent_id());
  }

  if (out.strategy() == EXEC_STRATEGY_UNSPECIFIED) {
    out.set_strategy(has_limit_price(out) ? EXEC_STRATEGY_LIMIT
                                          : EXEC_STRATEGY_MARKET);
  }

  if (out.tif() == TIF_UNSPECIFIED) {
    if (adapter != nullptr &&
        (adapter->Type() == domain::VenueType::kDex ||
         adapter->Type() == domain::VenueType::kAmm)) {
      out.set_tif(TIF_IOC);
    } else if (has_limit_price(out)) {
      out.set_tif(TIF_GTC);
    }
  }

  return out;
}

std::string ExecuteOnVenue::ResolveVenueSymbol(const ExecutionIntent& intent,
                                               const std::string& venue_id) const {
  if (!intent.venue_symbol().empty()) {
    return normalize_venue_symbol(intent.venue_symbol());
  }

  if (!venue_id.empty()) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto runtime_it = runtime_symbol_map_.find(venue_id);
    if (runtime_it != runtime_symbol_map_.end()) {
      return runtime_it->second;
    }
  }

  const std::string symbol = canonical_symbol(intent.instrument());
  if (!venue_id.empty() && !symbol.empty()) {
    auto it = symbol_map_.find(venue_id + "|" + symbol);
    if (it != symbol_map_.end()) return it->second;

    it = symbol_map_.find(venue_id + ":" + symbol);
    if (it != symbol_map_.end()) return it->second;
  }

  if (!symbol.empty()) {
    const auto it = symbol_map_.find(symbol);
    if (it != symbol_map_.end()) return it->second;
  }

  return default_venue_symbol(intent.instrument());
}

}  // namespace cex::venues::app
