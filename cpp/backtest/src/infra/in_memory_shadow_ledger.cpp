// ============================================================================
// in_memory_shadow_ledger.cpp — F-15 isolated ledger для replay sessions.
//
// Назначение и физический смысл:
//   Replay session нуждается в собственном accounting state, который НЕ
//   должен trogать live LedgerUseCases. Этот класс — in-memory shadow:
//   тот же API что у LedgerUseCases (ApplyBatchResult, GetBalances, etc.),
//   но всё persistent state живёт только в session lifetime.
//
//   ADR-015 isolation pattern: каждый replay run = свой ShadowLedger
//   instance. Между runs state не сохраняется (есть PG-backed replay_results
//   таблица для итоговых aggregates).
//
// Что хранит:
//   - balances per (user, currency).
//   - positions per (user, instrument).
//   - hedge_pnl_records.
//   - Idempotency set (batch_id) — защита от double-apply при replay reset.
//
// Не хранит:
//   - reservations (replay не делает reserve/release flow).
//   - PG persistence (всё in-memory).
//
// Scales (kQtyScale=8, kPriceScale=8, etc) — заведомо больше production
// (которое использует 2/8/18) — replay требует precision для accurate
// parity checks с production stored values.
// ============================================================================

#include "infra/in_memory_shadow_ledger.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <map>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "app/ledger_uc.hpp"
#include "cex/common/decimal.hpp"
#include "fob/ledger/v1/ledger.pb.h"
#include "fob/matching/v1/batch.pb.h"

namespace cex::backtest::infra {
namespace {

using cex::common::Decimal;
using cex::ledger::app::LedgerUseCases;

constexpr int32_t kQtyScale = 8;
constexpr int32_t kPriceScale = 8;
constexpr int32_t kNotionalScale = 8;
constexpr int32_t kFeeScale = 8;
constexpr int32_t kReportingScale = 8;

Decimal DecimalFromDouble(double value, int32_t scale) {
  const double factor = std::pow(10.0, static_cast<double>(scale));
  return Decimal{static_cast<int64_t>(std::llround(value * factor)), scale};
}

Decimal ParseDecimalString(const std::string& text) {
  if (text.empty()) return Decimal::zero();

  bool negative = false;
  bool seen_dot = false;
  int32_t scale = 0;
  int64_t units = 0;
  bool seen_digit = false;

  for (char ch : text) {
    if (ch == '+' || std::isspace(static_cast<unsigned char>(ch)) != 0) continue;
    if (ch == '-') {
      negative = true;
      continue;
    }
    if (ch == '.') {
      if (seen_dot) break;
      seen_dot = true;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) break;
    seen_digit = true;
    units = units * 10 + static_cast<int64_t>(ch - '0');
    if (seen_dot) ++scale;
  }

  if (!seen_digit) return Decimal::zero();
  if (negative) units = -units;
  return Decimal{units, scale};
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

fob::common::v1::Side ParseSide(const std::string& side) {
  const std::string normalized = LowerAscii(side);
  if (normalized == "sell") return fob::common::v1::SIDE_SELL;
  return fob::common::v1::SIDE_BUY;
}

fob::matching::v1::FlowFill ToProtoFill(const app::ShadowLedgerFill& fill) {
  fob::matching::v1::FlowFill proto;
  proto.set_order_id(fill.order_id);
  proto.set_user_id(fill.user_id);
  proto.mutable_instrument()->set_symbol(fill.symbol);
  proto.mutable_instrument()->set_base(fill.base);
  proto.mutable_instrument()->set_quote(fill.quote);
  proto.set_side(ParseSide(fill.side));
  *proto.mutable_executed_qty() = DecimalFromDouble(fill.executed_qty, kQtyScale).to_proto();
  *proto.mutable_price() = DecimalFromDouble(fill.price, kPriceScale).to_proto();
  *proto.mutable_executed_notional() =
      DecimalFromDouble(fill.executed_notional, kNotionalScale).to_proto();
  proto.set_liquidity_source(fill.liquidity_source);
  proto.mutable_provenance()->set_liquidity_source(fill.liquidity_source);
  proto.mutable_provenance()->set_venue_id(fill.venue_id);
  proto.mutable_provenance()->set_snapshot_id(fill.snapshot_id);
  proto.mutable_provenance()->set_curve_id(fill.curve_id);

  if (fill.fee_amount != 0.0 || !fill.fee_currency.empty()) {
    auto* fee = proto.mutable_fee();
    fee->set_fee_type("replay");
    fee->mutable_cost()->set_currency(fill.fee_currency);
    *fee->mutable_cost()->mutable_amount() =
        DecimalFromDouble(fill.fee_amount, kFeeScale).to_proto();
  }
  return proto;
}

std::unordered_map<std::string, Decimal> ToCurrentPrices(
    const std::map<std::string, double>& clear_prices) {
  std::unordered_map<std::string, Decimal> out;
  out.reserve(clear_prices.size());
  for (const auto& [symbol, price] : clear_prices) {
    out.emplace(symbol, DecimalFromDouble(price, kPriceScale));
  }
  return out;
}

Decimal LookupMarkPrice(const std::string& reporting_currency,
                        const std::string& symbol,
                        const std::string& base,
                        const std::string& quote,
                        const std::map<std::string, double>& clear_prices,
                        const Decimal& fallback) {
  auto it = clear_prices.find(symbol);
  if (it != clear_prices.end()) {
    return DecimalFromDouble(it->second, kPriceScale);
  }
  if (quote == reporting_currency && !base.empty()) {
    auto direct = clear_prices.find(base + "/" + reporting_currency);
    if (direct != clear_prices.end()) {
      return DecimalFromDouble(direct->second, kPriceScale);
    }
  }
  if (base == reporting_currency && !quote.empty()) {
    auto inverse = clear_prices.find(reporting_currency + "/" + quote);
    if (inverse != clear_prices.end() && inverse->second != 0.0) {
      return DecimalFromDouble(1.0 / inverse->second, kPriceScale);
    }
  }
  return fallback;
}

std::unique_ptr<LedgerUseCases> BuildLedgerFromState(
    const app::ShadowLedgerNamespaceState& state) {
  auto ledger = std::make_unique<LedgerUseCases>(
      LedgerUseCases::InitOptions{.seed_demo_state = false});

  const std::string tracked_user =
      state.tracked_user_id.empty() ? state.session_id : state.tracked_user_id;

  for (const auto& [currency, total_s] : state.balances) {
    const Decimal total = ParseDecimalString(total_s);
    const auto reserved_it = state.reserved_balances.find(currency);
    const Decimal reserved = reserved_it == state.reserved_balances.end()
        ? Decimal::zero() : ParseDecimalString(reserved_it->second);
    ledger->SeedBalance(tracked_user, currency, Decimal::sub(total, reserved), reserved);
  }
  for (const auto& [symbol, qty_s] : state.positions) {
    const Decimal qty = ParseDecimalString(qty_s);
    const auto avg_it = state.avg_entry_prices.find(symbol);
    const Decimal avg = avg_it == state.avg_entry_prices.end()
        ? Decimal::zero() : ParseDecimalString(avg_it->second);
    const auto pnl_it = state.realised_pnl_by_symbol.find(symbol);
    const Decimal pnl = pnl_it == state.realised_pnl_by_symbol.end()
        ? Decimal::zero() : ParseDecimalString(pnl_it->second);
    ledger->SeedPosition(tracked_user, symbol, qty, avg, pnl);
  }
  return ledger;
}

}  // namespace

InMemoryShadowLedger::InMemoryShadowLedger(app::ReplayRuntimeMetrics* metrics)
    : metrics_(metrics) {}

InMemoryShadowLedger::~InMemoryShadowLedger() = default;

bool InMemoryShadowLedger::NamespaceExists(const std::string& namespace_id) {
  std::lock_guard<std::mutex> lock(mu_);
  return namespaces_.find(namespace_id) != namespaces_.end();
}

bool InMemoryShadowLedger::CreateNamespace(
    const app::ShadowLedgerNamespaceState& initial) {
  if (initial.namespace_id.empty()) return false;

  NamespaceEntry entry;
  entry.state = initial;
  if (entry.state.reporting_currency.empty()) {
    entry.state.reporting_currency = "USDT";
  }
  entry.ledger = BuildLedgerFromState(entry.state);

  std::lock_guard<std::mutex> lock(mu_);
  auto [it, inserted] = namespaces_.emplace(initial.namespace_id, std::move(entry));
  (void)it;
  if (inserted && metrics_ != nullptr) {
    metrics_->SetShadowNamespacesActive(namespaces_.size());
  }
  return inserted;
}

std::optional<app::ShadowLedgerNamespaceState>
InMemoryShadowLedger::GetNamespace(const std::string& namespace_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = namespaces_.find(namespace_id);
  if (it == namespaces_.end()) return std::nullopt;
  return it->second.state;
}

app::ShadowLedgerStepState InMemoryShadowLedger::ApplyFills(
    const app::ShadowLedgerApplyRequest& request) {
  app::ShadowLedgerStepState step;
  step.namespace_id = request.namespace_id;
  step.batch_id = request.batch_id;

  std::lock_guard<std::mutex> lock(mu_);
  auto it = namespaces_.find(request.namespace_id);
  if (it == namespaces_.end()) {
    step.error_code = "missing_namespace";
    step.error_message = "shadow namespace does not exist";
    return step;
  }

  NamespaceEntry& entry = it->second;
  const std::string tracked_user = !request.tracked_user_id.empty()
      ? request.tracked_user_id
      : (!entry.state.tracked_user_id.empty() ? entry.state.tracked_user_id
                                              : entry.state.session_id);
  const std::string reporting_currency = !request.reporting_currency.empty()
      ? request.reporting_currency
      : (entry.state.reporting_currency.empty() ? "USDT"
                                                : entry.state.reporting_currency);

  step.tracked_user_id = tracked_user;
  step.reporting_currency = reporting_currency;

  if (tracked_user.empty()) {
    step.error_code = "missing_tracked_user";
    step.error_message = "tracked user id is required for shadow apply";
    return step;
  }
  if (entry.ledger == nullptr) {
    step.error_code = "ledger_not_initialized";
    step.error_message = "shadow ledger namespace has no backing ledger";
    return step;
  }
  if (request.batch_id.empty()) {
    step.error_code = "missing_batch_id";
    step.error_message = "batch_id is required for shadow apply";
    return step;
  }
  if (auto checkpoint_it = entry.checkpoints.find(request.batch_id);
      checkpoint_it != entry.checkpoints.end()) {
    return checkpoint_it->second.step;
  }

  const app::ShadowLedgerNamespaceState before_state = entry.state;

  fob::ledger::v1::ApplyBatchResultRequest apply_req;
  apply_req.mutable_batch()->set_batch_id(request.batch_id);
  std::size_t applied_count = 0;
  for (const auto& fill : request.fills) {
    if (fill.user_id != tracked_user) continue;
    *apply_req.mutable_batch()->add_fills() = ToProtoFill(fill);
    ++applied_count;
  }

  if (applied_count > 0) {
    auto apply_resp = entry.ledger->ApplyBatchResult(apply_req);
    if (!apply_resp.success()) {
      step.error_code = "apply_failed";
      step.error_message = apply_resp.has_error() ? apply_resp.error().message()
                                                  : "shadow ledger apply failed";
      entry.last_step = step;
      return step;
    }
  }

  fob::ledger::v1::GetBalancesRequest balances_req;
  balances_req.set_user_id(tracked_user);
  const auto balances_resp = entry.ledger->GetBalances(balances_req);
  const auto positions = entry.ledger->GetPositions(tracked_user);
  const auto current_prices = ToCurrentPrices(request.clear_prices);

  fob::ledger::v1::GetUnrealisedPnLRequest unreal_req;
  unreal_req.set_user_id(tracked_user);
  const auto unreal_resp = entry.ledger->GetUnrealisedPnL(unreal_req, current_prices);

  fob::ledger::v1::GetRealisedPnLRequest real_req;
  real_req.set_user_id(tracked_user);
  const auto real_resp = entry.ledger->GetRealisedPnL(real_req);

  Decimal realized_total = Decimal::zero();
  for (const auto& item : real_resp.positions()) {
    realized_total = Decimal::add(realized_total,
                                  Decimal::from_proto(item.total_realised_pnl()));
  }

  Decimal unrealized_total = Decimal::zero();
  for (const auto& item : unreal_resp.positions()) {
    unrealized_total = Decimal::add(unrealized_total,
                                    Decimal::from_proto(item.unrealised_pnl()));
  }

  Decimal initial_margin = Decimal::zero();
  Decimal maintenance_margin = Decimal::zero();
  for (const auto& [symbol, pos] : positions) {
    if (Decimal::cmp(pos.amount, Decimal::zero()) == 0) continue;

    const auto slash = symbol.find('/');
    const std::string base = slash == std::string::npos ? std::string{} : symbol.substr(0, slash);
    const std::string quote = slash == std::string::npos ? std::string{} : symbol.substr(slash + 1);
    const Decimal mark = LookupMarkPrice(
        reporting_currency, symbol, base, quote, request.clear_prices, pos.avg_entry_price);
    Decimal abs_qty = pos.amount;
    if (abs_qty.units < 0) abs_qty.units = -abs_qty.units;
    Decimal notional = Decimal::mul(abs_qty, mark);
    Decimal init = notional;
    init.units /= 10;
    Decimal maint = notional;
    maint.units /= 20;
    initial_margin = Decimal::add(initial_margin, init);
    maintenance_margin = Decimal::add(maintenance_margin, maint);
  }

  Decimal equity = Decimal::zero();
  for (const auto& balance : balances_resp.balances()) {
    const Decimal total = Decimal::from_proto(balance.total());
    entry.state.balances[balance.currency()] = total.to_string();
    entry.state.reserved_balances[balance.currency()] =
        Decimal::from_proto(balance.reserved()).to_string();

    if (balance.currency() == reporting_currency) {
      equity = Decimal::add(equity, total);
      continue;
    }

    auto direct = request.clear_prices.find(balance.currency() + "/" + reporting_currency);
    if (direct != request.clear_prices.end()) {
      equity = Decimal::add(equity, Decimal::mul(total, DecimalFromDouble(direct->second, kPriceScale)));
      continue;
    }

    auto inverse = request.clear_prices.find(reporting_currency + "/" + balance.currency());
    if (inverse != request.clear_prices.end() && inverse->second != 0.0) {
      equity = Decimal::add(equity, Decimal::mul(total, DecimalFromDouble(1.0 / inverse->second, kPriceScale)));
    }
  }

  entry.state.tracked_user_id = tracked_user;
  entry.state.reporting_currency = reporting_currency;
  entry.state.positions.clear();
  entry.state.avg_entry_prices.clear();
  entry.state.realised_pnl_by_symbol.clear();
  for (const auto& [symbol, pos] : positions) {
    entry.state.positions[symbol] = pos.amount.to_string();
    entry.state.avg_entry_prices[symbol] = pos.avg_entry_price.to_string();
    entry.state.realised_pnl_by_symbol[symbol] = pos.realised_pnl.to_string();
  }

  step.realized_pnl = realized_total.to_string();
  step.unrealized_pnl = unrealized_total.to_string();
  step.total_pnl = Decimal::add(realized_total, unrealized_total).to_string();
  step.initial_margin = initial_margin.to_string();
  step.maintenance_margin = maintenance_margin.to_string();
  step.equity = equity.to_string();
  step.balances = entry.state.balances;
  step.positions = entry.state.positions;
  step.avg_entry_prices = entry.state.avg_entry_prices;
  step.ok = true;

  app::ShadowLedgerBatchCheckpoint checkpoint;
  checkpoint.namespace_id = request.namespace_id;
  checkpoint.batch_id = request.batch_id;
  checkpoint.before_state = before_state;
  checkpoint.after_state = entry.state;
  checkpoint.step = step;
  entry.applied_batch_order.push_back(request.batch_id);
  entry.checkpoints[request.batch_id] = checkpoint;
  entry.last_step = step;
  return step;
}

std::optional<app::ShadowLedgerStepState> InMemoryShadowLedger::GetLastStep(
    const std::string& namespace_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = namespaces_.find(namespace_id);
  if (it == namespaces_.end()) return std::nullopt;
  return it->second.last_step;
}

std::optional<app::ShadowLedgerBatchCheckpoint> InMemoryShadowLedger::GetCheckpoint(
    const std::string& namespace_id,
    const std::string& batch_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto ns_it = namespaces_.find(namespace_id);
  if (ns_it == namespaces_.end()) return std::nullopt;
  auto cp_it = ns_it->second.checkpoints.find(batch_id);
  if (cp_it == ns_it->second.checkpoints.end()) return std::nullopt;
  return cp_it->second;
}

bool InMemoryShadowLedger::RestoreBeforeBatch(const std::string& namespace_id,
                                              const std::string& batch_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto ns_it = namespaces_.find(namespace_id);
  if (ns_it == namespaces_.end()) return false;

  NamespaceEntry& entry = ns_it->second;
  const auto order_it = std::find(
      entry.applied_batch_order.begin(), entry.applied_batch_order.end(), batch_id);
  if (order_it == entry.applied_batch_order.end()) return false;

  const std::size_t index =
      static_cast<std::size_t>(std::distance(entry.applied_batch_order.begin(), order_it));
  auto cp_it = entry.checkpoints.find(batch_id);
  if (cp_it == entry.checkpoints.end()) return false;

  entry.state = cp_it->second.before_state;
  entry.ledger = BuildLedgerFromState(entry.state);

  if (index == 0) {
    entry.last_step.reset();
  } else {
    const auto& prev_batch_id = entry.applied_batch_order[index - 1];
    auto prev_it = entry.checkpoints.find(prev_batch_id);
    entry.last_step = prev_it == entry.checkpoints.end()
        ? std::optional<app::ShadowLedgerStepState>{}
        : std::optional<app::ShadowLedgerStepState>{prev_it->second.step};
  }

  for (auto it = order_it; it != entry.applied_batch_order.end(); ++it) {
    entry.checkpoints.erase(*it);
  }
  entry.applied_batch_order.erase(order_it, entry.applied_batch_order.end());
  return true;
}

bool InMemoryShadowLedger::DropNamespace(const std::string& namespace_id) {
  std::lock_guard<std::mutex> lock(mu_);
  const bool erased = namespaces_.erase(namespace_id) > 0;
  if (erased && metrics_ != nullptr) {
    metrics_->SetShadowNamespacesActive(namespaces_.size());
  }
  return erased;
}

std::size_t InMemoryShadowLedger::NamespaceCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return namespaces_.size();
}

}  // namespace cex::backtest::infra
