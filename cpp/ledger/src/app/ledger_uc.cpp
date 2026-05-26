#include "app/ledger_uc.hpp"

#include <cmath>
#include <exception>
#include <sstream>
#include <utility>

#include "cex/common/log.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

namespace cex::ledger::app {

using cex::common::Decimal;

namespace {

bool has_non_empty_error(const fob::execution::v1::ExecutionReport& report) {
  return report.has_error() &&
      (!report.error().code().empty() ||
       !report.error().message().empty() ||
       !report.error().details().empty());
}

bool is_positive(const fob::common::v1::Decimal& value) {
  return Decimal::cmp(Decimal::from_proto(value), Decimal::zero()) > 0;
}

fob::execution::v1::ExecutionReportStatus normalize_execution_status(
    const fob::execution::v1::ExecutionReport& report) {
  const auto status = report.status();
  if (status != fob::execution::v1::EXECUTION_REPORT_STATUS_NEW &&
      status != fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED) {
    return status;
  }

  if (has_non_empty_error(report)) return fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
  if (is_positive(report.filled_qty()) && is_positive(report.remaining_qty())) {
    return fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
  }
  if (is_positive(report.filled_qty()) && !is_positive(report.remaining_qty())) {
    return fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
  }
  return status;
}

bool is_terminal_status(const fob::execution::v1::ExecutionReportStatus status) {
  switch (status) {
    case fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED:
      return true;
    default:
      return false;
  }
}

bool is_allowed_status_transition(const fob::execution::v1::ExecutionReportStatus prev,
                                  const fob::execution::v1::ExecutionReportStatus next) {
  if (prev == fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED) return true;
  if (prev == next) return true;
  if (is_terminal_status(prev)) return false;
  if (prev == fob::execution::v1::EXECUTION_REPORT_STATUS_NEW) {
    return next == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED;
  }
  if (prev == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED) {
    return next == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
  }
  return false;
}

std::string normalize_symbol(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c == '/' || c == ' ' || c == '\t' || c == '\n') continue;
    out.push_back(c);
  }
  return out;
}

std::pair<std::string, std::string> extract_base_quote(
    const fob::common::v1::Instrument& instrument) {
  if (!instrument.base().empty() || !instrument.quote().empty()) {
    return {instrument.base(), instrument.quote()};
  }
  const std::string symbol = instrument.symbol();
  const std::size_t slash = symbol.find('/');
  if (slash == std::string::npos) return {"", ""};
  return {symbol.substr(0, slash), symbol.substr(slash + 1)};
}

bool instrument_matches(const fob::execution::v1::ExecutionIntent& intent,
                        const fob::execution::v1::ExecutionReport& report) {
  const auto& lhs = intent.instrument();
  const auto& rhs = report.instrument();
  if (!lhs.symbol().empty() && !rhs.symbol().empty() && lhs.symbol() != rhs.symbol()) {
    return false;
  }
  if (!lhs.base().empty() && !rhs.base().empty() && lhs.base() != rhs.base()) return false;
  if (!lhs.quote().empty() && !rhs.quote().empty() && lhs.quote() != rhs.quote()) return false;
  return true;
}

std::string execution_state_key(const fob::execution::v1::ExecutionReport& report) {
  const std::string order_key =
      !report.venue_order_id().empty() ? report.venue_order_id() : report.client_order_id();
  return report.intent_id() + "|" + order_key;
}

std::string execution_report_key(const fob::execution::v1::ExecutionReport& report) {
  if (!report.report_id().empty()) return report.report_id();
  std::ostringstream out;
  out << report.intent_id() << '|'
      << report.venue_order_id() << '|'
      << static_cast<int>(normalize_execution_status(report)) << '|'
      << report.filled_qty().units() << ':' << report.filled_qty().scale() << '|'
      << report.remaining_qty().units() << ':' << report.remaining_qty().scale() << '|'
      << report.average_price().units() << ':' << report.average_price().scale();
  return out.str();
}

google::protobuf::Timestamp report_timestamp(const fob::execution::v1::ExecutionReport& report) {
  if (report.meta().has_ts_event()) return report.meta().ts_event();
  return cex::common::now_ts();
}

std::chrono::system_clock::time_point timestamp_to_time_point(
    const google::protobuf::Timestamp& ts) {
  const auto duration = std::chrono::seconds{ts.seconds()} +
                        std::chrono::nanoseconds{ts.nanos()};
  return std::chrono::system_clock::time_point{
      std::chrono::duration_cast<std::chrono::system_clock::duration>(duration)};
}

}  // namespace

LedgerUseCases::LedgerUseCases() : LedgerUseCases(InitOptions{}, nullptr, nullptr) {}

LedgerUseCases::LedgerUseCases(InitOptions options)
    : LedgerUseCases(options, nullptr, nullptr) {}

LedgerUseCases::LedgerUseCases(
    std::shared_ptr<PositionsRepositoryPort> positions_repo,
    std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo)
    : LedgerUseCases(InitOptions{}, std::move(positions_repo), std::move(entries_repo)) {}

LedgerUseCases::LedgerUseCases(
    InitOptions options,
    std::shared_ptr<PositionsRepositoryPort> positions_repo,
    std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo)
    : positions_repo_(std::move(positions_repo)),
      entries_repo_(std::move(entries_repo)),
      idempotency_repo_(nullptr),
      hedge_entries_repo_(nullptr) {
  if (!options.seed_demo_state) return;

  // Demo initial balances so you can place orders immediately.
  // user "demo-user" has 10k USDT and 1 BTC.
  std::lock_guard<std::mutex> lg(mu_);

  balances_["demo-user"]["USDT"].available = Decimal{10'00000, 2}; // 10000.00 USDT
  balances_["demo-user"]["USDT"].reserved  = Decimal{0, 2};

  balances_["demo-user"]["BTC"].available = Decimal{2'50000000, 8}; // 2.5 BTC seed to match UI expectation
  balances_["demo-user"]["BTC"].reserved  = Decimal{0, 8};

  // Initialize demo-user position for BTC
  positions_["demo-user"]["BTC/USDT"].amount = Decimal{1'00000000, 8};
  positions_["demo-user"]["BTC/USDT"].avg_entry_price = Decimal{500000, 2}; // 5000.00 USDT
  positions_["demo-user"]["BTC/USDT"].realised_pnl = Decimal{0, 2};
}

LedgerUseCases::LedgerUseCases(
    InitOptions options,
    std::shared_ptr<PositionsRepositoryPort> positions_repo,
    std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo,
    std::shared_ptr<HedgeLedgerEntriesRepositoryPort> hedge_entries_repo)
    : positions_repo_(std::move(positions_repo)),
      entries_repo_(std::move(entries_repo)),
      idempotency_repo_(nullptr),
      hedge_entries_repo_(std::move(hedge_entries_repo)) {
  if (!options.seed_demo_state) return;

  std::lock_guard<std::mutex> lg(mu_);

  balances_["demo-user"]["USDT"].available = Decimal{10'00000, 2};
  balances_["demo-user"]["USDT"].reserved  = Decimal{0, 2};

  balances_["demo-user"]["BTC"].available = Decimal{2'50000000, 8};
  balances_["demo-user"]["BTC"].reserved  = Decimal{0, 8};

  positions_["demo-user"]["BTC/USDT"].amount = Decimal{1'00000000, 8};
  positions_["demo-user"]["BTC/USDT"].avg_entry_price = Decimal{500000, 2};
  positions_["demo-user"]["BTC/USDT"].realised_pnl = Decimal{0, 2};
}

void LedgerUseCases::SeedBalance(const std::string& user_id,
                                 const std::string& currency,
                                 const Decimal& available,
                                 const Decimal& reserved) {
  std::lock_guard<std::mutex> lg(mu_);
  auto& bal = ensure_balance_locked(user_id, currency);
  bal.available = available;
  bal.reserved = reserved;
}

void LedgerUseCases::SeedPosition(const std::string& user_id,
                                  const std::string& instrument_symbol,
                                  const Decimal& amount,
                                  const Decimal& avg_entry_price,
                                  const Decimal& realised_pnl) {
  std::lock_guard<std::mutex> lg(mu_);
  auto& pos = ensure_position_locked(user_id, instrument_symbol);
  pos.amount = amount;
  pos.avg_entry_price = avg_entry_price;
  pos.realised_pnl = realised_pnl;
}

LedgerUseCases::Balance& LedgerUseCases::ensure_balance_locked(
    const std::string& user, const std::string& currency) {
  // Ensure both available/reserved exist with the same scale as existing values.
  auto& ub = balances_[user];
  auto& b = ub[currency];
  return b;
}

LedgerUseCases::Position& LedgerUseCases::ensure_position_locked(
    const std::string& user, const std::string& instrument) {
  auto& up = positions_[user];
  auto& pos = up[instrument];
  // Initialize if needed
  const Decimal zero{0, 0};
  if (pos.amount.scale == 0 && pos.amount.units == 0) {
    pos.amount = zero;
    pos.avg_entry_price = zero;
    pos.realised_pnl = zero;
  }
  return pos;
}

void LedgerUseCases::calculate_and_record_pnl(
    const std::string& user,
    const std::string& instrument,
    const Decimal& sell_qty,
    const Decimal& sell_price) {
  
  Position& pos = ensure_position_locked(user, instrument);
  
  // Only long positions have positive amount
  if (Decimal::cmp(pos.amount, Decimal{0, pos.amount.scale}) <= 0) {
    // No position to sell from (short selling not implemented yet)
    cex::common::log_json("WARN", "Cannot sell without long position",
                          {{"user", user},
                           {"instrument", instrument},
                           {"position_amount", pos.amount.to_string()}});
    return;
  }
  
  // Calculate how much we can sell from this position
  Decimal sellable = Decimal::min(sell_qty, pos.amount);
  
  // Convert to double for PnL calculation
  double sell_qty_d = static_cast<double>(sellable.units) / std::pow(10.0, sellable.scale);
  double sell_price_d = static_cast<double>(sell_price.units) / std::pow(10.0, sell_price.scale);
  double avg_price_d = static_cast<double>(pos.avg_entry_price.units) / std::pow(10.0, pos.avg_entry_price.scale);
  
  // PnL = (sell_price - avg_entry_price) * sell_qty
  double pnl_d = (sell_price_d - avg_price_d) * sell_qty_d;
  
  // Convert back to Decimal with scale 2 (USDT)
  int64_t pnl_units = static_cast<int64_t>(std::llround(pnl_d * 100.0));
  Decimal pnl{pnl_units, 2};
  
  // Update realised PnL
  pos.realised_pnl = Decimal::add(pos.realised_pnl, pnl);
  
  // Reduce position amount
  double remaining_d = (static_cast<double>(pos.amount.units) / std::pow(10.0, pos.amount.scale)) - sell_qty_d;
  int64_t remaining_units = static_cast<int64_t>(std::llround(remaining_d * std::pow(10.0, pos.amount.scale)));
  pos.amount = Decimal{remaining_units, pos.amount.scale};
  
  // If position fully closed, reset avg price
  if (Decimal::cmp(pos.amount, Decimal{0, pos.amount.scale}) == 0) {
    pos.avg_entry_price = Decimal{0, pos.avg_entry_price.scale};
  }
  
  cex::common::log_json("INFO", "Realised PnL calculated",
                        {{"user", user},
                         {"instrument", instrument},
                         {"sell_qty", sellable.to_string()},
                         {"sell_price", sell_price.to_string()},
                         {"avg_entry_price", pos.avg_entry_price.to_string()},
                         {"pnl", pnl.to_string()},
                         {"cumulative_pnl", pos.realised_pnl.to_string()},
                         {"remaining_position", pos.amount.to_string()}});
}

void LedgerUseCases::update_position_for_fill(const fob::matching::v1::FlowFill& fill) {
  const std::string user = fill.user_id();
  const std::string instrument = fill.instrument().symbol();
  Decimal qty = Decimal::from_proto(fill.executed_qty());
  Decimal price = Decimal::from_proto(fill.price());
  
  Position& pos = ensure_position_locked(user, instrument);
  
  if (fill.side() == fob::common::v1::SIDE_BUY) {
    // BUY: increase position, update average entry price
    
    // Convert to double for calculation
    double old_amount_d = static_cast<double>(pos.amount.units) / std::pow(10.0, pos.amount.scale);
    double old_avg_d = static_cast<double>(pos.avg_entry_price.units) / std::pow(10.0, pos.avg_entry_price.scale);
    double qty_d = static_cast<double>(qty.units) / std::pow(10.0, qty.scale);
    double price_d = static_cast<double>(price.units) / std::pow(10.0, price.scale);
    
    double old_value_d = old_amount_d * old_avg_d;
    double new_value_d = qty_d * price_d;
    double total_value_d = old_value_d + new_value_d;
    double total_amount_d = old_amount_d + qty_d;
    
    if (total_amount_d > 1e-12) {
      double avg_price_d = total_value_d / total_amount_d;
      // Store with scale 2 (USDT price precision)
      int64_t avg_units = static_cast<int64_t>(std::llround(avg_price_d * 100.0));
      pos.avg_entry_price = Decimal{avg_units, 2};
    } else {
      pos.avg_entry_price = Decimal{0, 2};
    }
    
    // Store amount with original qty scale
    int64_t new_amount_units = static_cast<int64_t>(std::llround(total_amount_d * std::pow(10.0, qty.scale)));
    pos.amount = Decimal{new_amount_units, qty.scale};
    
    cex::common::log_json("INFO", "Position increased (BUY)",
                          {{"user", user},
                           {"instrument", instrument},
                           {"qty", qty.to_string()},
                           {"price", price.to_string()},
                           {"new_amount", pos.amount.to_string()},
                           {"new_avg_price", pos.avg_entry_price.to_string()}});
  } else if (fill.side() == fob::common::v1::SIDE_SELL) {
    // SELL: calculate PnL and decrease position
    calculate_and_record_pnl(user, instrument, qty, price);
  }
}

fob::ledger::v1::GetBalancesResponse LedgerUseCases::GetBalances(
    const fob::ledger::v1::GetBalancesRequest& req) {
  fob::ledger::v1::GetBalancesResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);
  auto it_user = balances_.find(req.user_id());
  if (it_user == balances_.end()) return resp;

  for (const auto& [ccy, bal] : it_user->second) {
    auto* out = resp.add_balances();
    out->set_currency(ccy);
    *out->mutable_available() = bal.available.to_proto();
    *out->mutable_reserved() = bal.reserved.to_proto();
    *out->mutable_total() = Decimal::add(bal.available, bal.reserved).to_proto();
  }
  return resp;
}

fob::ledger::v1::ReserveFundsResponse LedgerUseCases::ReserveFunds(
    const fob::ledger::v1::ReserveFundsRequest& req) {
  fob::ledger::v1::ReserveFundsResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);

  // Idempotency: if reservation exists, return success (MVP).
  if (reservations_.count(req.reservation_id())) {
    resp.set_success(true);
    *resp.mutable_reserved_amount() = reservations_[req.reservation_id()].amount.to_proto();
    return resp;
  }

  auto& b = ensure_balance_locked(req.user_id(), req.currency());

  Decimal want = Decimal::from_proto(req.amount());
  // Align scales for comparison
  if (Decimal::cmp(b.available, want) < 0) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("INSUFFICIENT_FUNDS");
    e->set_message("Not enough available balance to reserve.");
    return resp;
  }

  b.available = Decimal::sub(b.available, want);
  b.reserved  = Decimal::add(b.reserved, want);

  reservations_[req.reservation_id()] = Reservation{
      .user_id=req.user_id(),
      .order_id=req.order_id(),
      .currency=req.currency(),
      .amount=want,
  };

  resp.set_success(true);
  *resp.mutable_reserved_amount() = want.to_proto();

  cex::common::log_json("INFO", "Reserved funds",
                        {{"user", req.user_id()},
                         {"currency", req.currency()},
                         {"amount", want.to_string()},
                         {"reservation_id", req.reservation_id()}});
  return resp;
}

void LedgerUseCases::ReleaseFunds(const fob::ledger::v1::ReleaseFundsRequest& req) {
  std::lock_guard<std::mutex> lg(mu_);

  auto it = reservations_.find(req.reservation_id());
  if (it == reservations_.end()) {
    cex::common::log_json("WARN", "ReleaseFunds: reservation not found",
                          {{"reservation_id", req.reservation_id()}});
    return;
  }

  auto& b = ensure_balance_locked(it->second.user_id, it->second.currency);
  b.reserved = Decimal::sub(b.reserved, it->second.amount);
  b.available = Decimal::add(b.available, it->second.amount);

  cex::common::log_json("INFO", "Released funds",
                        {{"user", it->second.user_id},
                         {"currency", it->second.currency},
                         {"amount", it->second.amount.to_string()},
                         {"reservation_id", req.reservation_id()}});
  reservations_.erase(it);
}

fob::ledger::v1::ApplyBatchResultResponse LedgerUseCases::ApplyBatchResult(
    const fob::ledger::v1::ApplyBatchResultRequest& req) {
  fob::ledger::v1::ApplyBatchResultResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  const std::string batch_id = req.batch().batch_id();

  if (idempotency_repo_ && idempotency_repo_->IsBatchProcessed(batch_id)) {
    cex::common::log_json("INFO", "Batch already processed, skipping",
                          {{"batch_id", batch_id}});
    resp.set_success(true);
    return resp;
  }

  {
    std::lock_guard<std::mutex> lg(mu_);

    // For each internal fill:
    // BUY: spend quote (reserved), receive base (available).
    // SELL: spend base (reserved), receive quote (available).
    for (const auto& fill : req.batch().fills()) {
      const std::string user = fill.user_id();
      const std::string base = fill.instrument().base();
      const std::string quote = fill.instrument().quote();

      Decimal qty = Decimal::from_proto(fill.executed_qty());
      Decimal notional = Decimal::from_proto(fill.executed_notional());
      auto reservation_it = reservations_.find(fill.order_id());

      if (fill.side() == fob::common::v1::SIDE_BUY) {
        // Decrease reserved quote by executed notional.
        auto& qbal = ensure_balance_locked(user, quote);
        qbal.reserved = Decimal::sub(qbal.reserved, notional);
        if (reservation_it != reservations_.end()) {
          reservation_it->second.amount = Decimal::sub(reservation_it->second.amount, notional);
        }

        // Increase available base by executed qty.
        auto& bbal = ensure_balance_locked(user, base);
        bbal.available = Decimal::add(bbal.available, qty);
      } else if (fill.side() == fob::common::v1::SIDE_SELL) {
        // Decrease reserved base by executed qty.
        auto& bbal = ensure_balance_locked(user, base);
        bbal.reserved = Decimal::sub(bbal.reserved, qty);
        if (reservation_it != reservations_.end()) {
          reservation_it->second.amount = Decimal::sub(reservation_it->second.amount, qty);
        }

        // Increase available quote by executed notional.
        auto& qbal = ensure_balance_locked(user, quote);
        qbal.available = Decimal::add(qbal.available, notional);
      }

      if (reservation_it != reservations_.end() &&
          Decimal::cmp(reservation_it->second.amount,
                       Decimal{0, reservation_it->second.amount.scale}) <= 0) {
        reservations_.erase(reservation_it);
      }
      
      update_position_for_fill(fill);

      if (fill.has_fee() && fill.fee().has_cost() && fill.fee().cost().has_amount()) {
        Decimal fee_amount = Decimal::from_proto(fill.fee().cost().amount());
        std::string fee_currency = fill.fee().cost().currency();
        if (fee_currency.empty()) fee_currency = quote;

        auto& fee_bal = ensure_balance_locked(user, fee_currency);
        fee_bal.available = Decimal::sub(fee_bal.available, fee_amount);

        if (fee_currency == quote) {
          auto& pos = ensure_position_locked(user, fill.instrument().symbol());
          pos.realised_pnl = Decimal::sub(pos.realised_pnl, fee_amount);
        }
      }
    }
  }

  for (const auto& fill : req.batch().fills()) {
    if (positions_repo_) {
      try {
        positions_repo_->ApplyFill(fill);
      } catch (const std::exception& ex) {
        cex::common::log_json(
            "ERROR", "PositionsRepositoryPort::ApplyFill failed",
            {{"batch_id", batch_id},
             {"order_id", fill.order_id()},
             {"error", ex.what()}});
      }
    }

    if (entries_repo_) {
      try {
        entries_repo_->CreateEntriesForFill(batch_id, fill);
      } catch (const std::exception& ex) {
        cex::common::log_json(
            "ERROR", "LedgerEntriesRepositoryPort::CreateEntriesForFill failed",
            {{"batch_id", batch_id},
             {"order_id", fill.order_id()},
             {"error", ex.what()}});
      }
    }
  }

  if (idempotency_repo_) {
    try {
      idempotency_repo_->MarkBatchProcessed(batch_id);
      cex::common::log_json("INFO", "Batch marked as processed",
                            {{"batch_id", batch_id}});
    } catch (const std::exception& e) {
      cex::common::log_json("ERROR", "Failed to mark batch as processed",
                            {{"batch_id", batch_id}, {"error", e.what()}});
    }
  }

  resp.set_success(true);
  return resp;
}

void LedgerUseCases::RememberExecutionIntent(const fob::execution::v1::ExecutionIntent& intent) {
  if (intent.intent_id().empty()) return;

  std::lock_guard<std::mutex> lg(mu_);
  execution_intents_[intent.intent_id()] = intent;

  auto pending_it = pending_execution_reports_.find(intent.intent_id());
  if (pending_it == pending_execution_reports_.end()) return;

  auto pending = std::move(pending_it->second);
  pending_execution_reports_.erase(pending_it);
  for (const auto& report : pending) {
    ++exec_recon_stats_.replayed_reports;
    (void)apply_execution_report_locked(report);
  }
}

void LedgerUseCases::ApplyExecutionReport(const fob::ledger::v1::ApplyExecutionReportRequest& req) {
  const auto& report = req.report();
  const std::string report_key = execution_report_key(report);

  std::lock_guard<std::mutex> lg(mu_);
  if (!seen_execution_report_keys_.insert(report_key).second) {
    ++exec_recon_stats_.duplicate_reports;
    cex::common::log_json("INFO", "Execution report already processed",
                          {{"report_key", report_key},
                           {"intent_id", report.intent_id()}});
    return;
  }

  if (report.intent_id().empty() ||
      execution_intents_.find(report.intent_id()) == execution_intents_.end()) {
    pending_execution_reports_[report.intent_id()].push_back(report);
    ++exec_recon_stats_.missing_plan_reports;
    ++exec_recon_stats_.queued_reports;
    cex::common::log_json("WARN", "Execution report queued until intent is known",
                          {{"intent_id", report.intent_id()},
                           {"report_id", report.report_id()},
                           {"venue", report.venue()}});
    return;
  }

  (void)apply_execution_report_locked(report);
}

LedgerUseCases::ExecutionReconciliationStats
LedgerUseCases::GetExecutionReconciliationStats() const {
  std::lock_guard<std::mutex> lg(mu_);
  return exec_recon_stats_;
}

LedgerUseCases::Position LedgerUseCases::GetPosition(const std::string& user_id, const std::string& instrument_symbol) const {
  std::lock_guard<std::mutex> lg(mu_);
  
  auto user_it = positions_.find(user_id);
  if (user_it == positions_.end()) {
    return Position{};
  }
  
  auto pos_it = user_it->second.find(instrument_symbol);
  if (pos_it == user_it->second.end()) {
    return Position{};
  }
  
  return pos_it->second;
}

std::unordered_map<std::string, LedgerUseCases::Position> LedgerUseCases::GetPositions(
    const std::string& user_id) const {
  std::lock_guard<std::mutex> lg(mu_);
  auto user_it = positions_.find(user_id);
  if (user_it == positions_.end()) return {};
  return user_it->second;
}

fob::ledger::v1::GetUnrealisedPnLResponse LedgerUseCases::GetUnrealisedPnL(
    const fob::ledger::v1::GetUnrealisedPnLRequest& req,
    const std::unordered_map<std::string, Decimal>& current_prices) {
  
  fob::ledger::v1::GetUnrealisedPnLResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);
  
  auto user_it = positions_.find(req.user_id());
  if (user_it == positions_.end()) {
    return resp;
  }
  
  for (const auto& [instrument, pos] : user_it->second) {
    // Skip if position is zero or negligible
    if (Decimal::cmp(pos.amount, Decimal{0, pos.amount.scale}) == 0) {
      continue;
    }
    
    // Filter by instrument if specified
    if (!req.instrument_symbol().empty() && instrument != req.instrument_symbol()) {
      continue;
    }
    
    // Get current price
    Decimal current_price;
    std::string price_source;
    
    auto price_it = current_prices.find(instrument);
    if (price_it != current_prices.end()) {
      current_price = price_it->second;
      price_source = "provided";
    } else {
      // Fallback: use avg_entry_price (unrealised PnL = 0)
      current_price = pos.avg_entry_price;
      price_source = "fallback_to_avg";
    }
    
    // Calculate unrealised PnL = (current_price - avg_entry_price) * position_amount
    Decimal price_diff = Decimal::sub(current_price, pos.avg_entry_price);
    Decimal unrealised = Decimal::mul(price_diff, pos.amount);
    
    auto* out = resp.add_positions();
    out->set_instrument_symbol(instrument);
    *out->mutable_position_amount() = pos.amount.to_proto();
    *out->mutable_avg_entry_price() = pos.avg_entry_price.to_proto();
    *out->mutable_current_price() = current_price.to_proto();
    *out->mutable_unrealised_pnl() = unrealised.to_proto();
    out->set_price_source(price_source);
    
    cex::common::log_json("DEBUG", "Unrealised PnL calculated",
                          {{"user", req.user_id()},
                           {"instrument", instrument},
                           {"position", pos.amount.to_string()},
                           {"avg_price", pos.avg_entry_price.to_string()},
                           {"current_price", current_price.to_string()},
                           {"unrealised_pnl", unrealised.to_string()}});
  }
  
  return resp;
}

fob::ledger::v1::GetRealisedPnLResponse LedgerUseCases::GetRealisedPnL(
    const fob::ledger::v1::GetRealisedPnLRequest& req) {
  
  fob::ledger::v1::GetRealisedPnLResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);
  
  auto user_it = positions_.find(req.user_id());
  if (user_it == positions_.end()) {
    return resp;
  }
  
  for (const auto& [instrument, pos] : user_it->second) {
    // Filter by instrument if specified
    if (!req.instrument_symbol().empty() && instrument != req.instrument_symbol()) {
      continue;
    }
    
    // Skip if no realised PnL
    if (Decimal::cmp(pos.realised_pnl, Decimal{0, pos.realised_pnl.scale}) == 0) {
      continue;
    }
    
    auto* out = resp.add_positions();
    out->set_instrument_symbol(instrument);
    *out->mutable_total_realised_pnl() = pos.realised_pnl.to_proto();
    // trade_count would require storing trade history separately
    out->set_trade_count(0);  // TODO: implement trade counting
    
    cex::common::log_json("DEBUG", "Realised PnL queried",
                          {{"user", req.user_id()},
                           {"instrument", instrument},
                           {"realised_pnl", pos.realised_pnl.to_string()}});
  }
  
  return resp;
}

LedgerUseCases::VenueBalanceEntry& LedgerUseCases::ensure_venue_balance_locked(
    const std::string& venue, const std::string& currency) {
  auto& vb = venue_balances_[venue];
  auto& entry = vb[currency];
  // Initialize if needed
  const Decimal zero{0, 0};
  if (entry.total.scale == 0 && entry.total.units == 0) {
    entry.total = zero;
    entry.reserved = zero;
    entry.available = zero;
    entry.updated_at = std::chrono::system_clock::now();
  }
  return entry;
}

fob::ledger::v1::GetVenueBalancesResponse LedgerUseCases::GetVenueBalances(
    const fob::ledger::v1::GetVenueBalancesRequest& req) {
  
  fob::ledger::v1::GetVenueBalancesResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);
  
  for (const auto& [venue, currencies] : venue_balances_) {
    // Filter by venue if specified
    if (!req.venue().empty() && venue != req.venue()) {
      continue;
    }
    
    for (const auto& [currency, entry] : currencies) {
      // Filter by currency if specified
      if (!req.currency().empty() && currency != req.currency()) {
        continue;
      }
      
      auto* out = resp.add_balances();
      out->set_venue(venue);
      out->set_currency(currency);
      *out->mutable_total() = entry.total.to_proto();
      *out->mutable_reserved() = entry.reserved.to_proto();
      *out->mutable_available() = entry.available.to_proto();
      
      auto* ts = out->mutable_updated_at();
      auto secs = std::chrono::duration_cast<std::chrono::seconds>(
          entry.updated_at.time_since_epoch());
      ts->set_seconds(secs.count());
    }
  }
  
  cex::common::log_json("INFO", "GetVenueBalances",
                        {{"venue_filter", req.venue()},
                         {"currency_filter", req.currency()},
                         {"results_count", std::to_string(resp.balances_size())}});
  
  return resp;
}

fob::ledger::v1::UpdateVenueBalanceResponse LedgerUseCases::UpdateVenueBalance(
    const fob::ledger::v1::UpdateVenueBalanceRequest& req) {
  
  fob::ledger::v1::UpdateVenueBalanceResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  if (req.venue().empty()) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("INVALID_VENUE");
    e->set_message("Venue name cannot be empty");
    return resp;
  }
  
  if (req.currency().empty()) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("INVALID_CURRENCY");
    e->set_message("Currency cannot be empty");
    return resp;
  }

  std::lock_guard<std::mutex> lg(mu_);
  
  auto& entry = ensure_venue_balance_locked(req.venue(), req.currency());
  
  Decimal new_total = Decimal::from_proto(req.total());
  Decimal new_reserved = Decimal::from_proto(req.reserved());
  Decimal new_available = Decimal::sub(new_total, new_reserved);
  
  entry.total = new_total;
  entry.reserved = new_reserved;
  entry.available = new_available;
  entry.updated_at = std::chrono::system_clock::now();
  
  resp.set_success(true);
  
  cex::common::log_json("INFO", "Updated venue balance",
                        {{"venue", req.venue()},
                         {"currency", req.currency()},
                         {"total", new_total.to_string()},
                         {"reserved", new_reserved.to_string()},
                         {"available", new_available.to_string()}});
  
  return resp;
}

cex::common::Decimal LedgerUseCases::calculate_hedge_pnl(
    fob::common::v1::Side side,
    const Decimal& executed_price,
    const Decimal& internal_price,
    const Decimal& qty) {
  
  // Price difference: external - internal
  Decimal price_diff = Decimal::sub(executed_price, internal_price);
  
  // PnL = price_diff * qty
  Decimal pnl = Decimal::mul(price_diff, qty);
  
  // For BUY on external venue: profit when external price < internal price
  // So we need to flip the sign for BUY orders
  if (side == fob::common::v1::SIDE_BUY) {
    // pnl = -pnl  (multiply by -1)
    pnl.units = -pnl.units;
  }
  
  return pnl;
}

bool LedgerUseCases::apply_execution_report_locked(
    const fob::execution::v1::ExecutionReport& report) {
  auto plan_it = execution_intents_.find(report.intent_id());
  if (plan_it == execution_intents_.end()) return false;
  const auto& intent = plan_it->second;

  if (!intent.venue().empty() && !report.venue().empty() &&
      intent.venue() != report.venue()) {
    ++exec_recon_stats_.plan_mismatch_reports;
    cex::common::log_json("WARN", "Execution report venue mismatch",
                          {{"intent_id", report.intent_id()},
                           {"plan_venue", intent.venue()},
                           {"report_venue", report.venue()}});
    return false;
  }

  if (!instrument_matches(intent, report)) {
    ++exec_recon_stats_.plan_mismatch_reports;
    cex::common::log_json("WARN", "Execution report instrument mismatch",
                          {{"intent_id", report.intent_id()},
                           {"plan_symbol", intent.instrument().symbol()},
                           {"report_symbol", report.instrument().symbol()}});
    return false;
  }

  if (!intent.venue_symbol().empty() && !report.venue_symbol().empty() &&
      normalize_symbol(intent.venue_symbol()) != normalize_symbol(report.venue_symbol())) {
    ++exec_recon_stats_.plan_mismatch_reports;
    cex::common::log_json("WARN", "Execution report venue symbol mismatch",
                          {{"intent_id", report.intent_id()},
                           {"plan_venue_symbol", intent.venue_symbol()},
                           {"report_venue_symbol", report.venue_symbol()}});
    return false;
  }

  if (!intent.client_order_id().empty() && !report.client_order_id().empty() &&
      intent.client_order_id() != report.client_order_id()) {
    ++exec_recon_stats_.plan_mismatch_reports;
    cex::common::log_json("WARN", "Execution report client_order_id mismatch",
                          {{"intent_id", report.intent_id()},
                           {"plan_client_order_id", intent.client_order_id()},
                           {"report_client_order_id", report.client_order_id()}});
    return false;
  }

  const Decimal target_qty = Decimal::from_proto(intent.target_qty());
  const Decimal filled_qty = Decimal::from_proto(report.filled_qty());
  const Decimal remaining_qty = Decimal::from_proto(report.remaining_qty());
  const Decimal average_price = Decimal::from_proto(report.average_price());
  const auto status = normalize_execution_status(report);

  if (Decimal::cmp(filled_qty, target_qty) > 0) {
    ++exec_recon_stats_.plan_mismatch_reports;
    cex::common::log_json("WARN", "Execution report filled_qty exceeds plan",
                          {{"intent_id", report.intent_id()},
                           {"filled_qty", filled_qty.to_string()},
                           {"target_qty", target_qty.to_string()}});
    return false;
  }

  auto& state = execution_states_[execution_state_key(report)];
  if (Decimal::cmp(filled_qty, state.filled_qty) < 0) {
    ++exec_recon_stats_.qty_regression_reports;
    cex::common::log_json("WARN", "Execution report filled_qty regressed",
                          {{"intent_id", report.intent_id()},
                           {"prev_filled_qty", state.filled_qty.to_string()},
                           {"filled_qty", filled_qty.to_string()}});
    return false;
  }

  if (!is_allowed_status_transition(state.status, status)) {
    ++exec_recon_stats_.status_regression_reports;
    cex::common::log_json("WARN", "Execution report status regressed",
                          {{"intent_id", report.intent_id()},
                           {"prev_status", std::to_string(state.status)},
                           {"status", std::to_string(status)}});
    return false;
  }

  const Decimal delta_qty = Decimal::sub(filled_qty, state.filled_qty);
  if (Decimal::cmp(delta_qty, Decimal::zero()) > 0) {
    auto [base_ccy, quote_ccy] = extract_base_quote(intent.instrument());
    if (base_ccy.empty() || quote_ccy.empty()) {
      auto from_report = extract_base_quote(report.instrument());
      if (base_ccy.empty()) base_ccy = from_report.first;
      if (quote_ccy.empty()) quote_ccy = from_report.second;
    }

    const Decimal internal_price =
        (intent.has_limit_price() && intent.limit_price().units() != 0)
            ? Decimal::from_proto(intent.limit_price())
            : average_price;
    const Decimal quote_delta = Decimal::mul(delta_qty, average_price);

    if (!base_ccy.empty()) {
      auto& base = ensure_venue_balance_locked(report.venue(), base_ccy);
      if (intent.side() == fob::common::v1::SIDE_BUY) {
        base.total = Decimal::add(base.total, delta_qty);
      } else if (intent.side() == fob::common::v1::SIDE_SELL) {
        base.total = Decimal::sub(base.total, delta_qty);
      }
      base.available = Decimal::sub(base.total, base.reserved);
      base.updated_at = std::chrono::system_clock::now();
    }

    if (!quote_ccy.empty()) {
      auto& quote = ensure_venue_balance_locked(report.venue(), quote_ccy);
      if (intent.side() == fob::common::v1::SIDE_BUY) {
        quote.total = Decimal::sub(quote.total, quote_delta);
      } else if (intent.side() == fob::common::v1::SIDE_SELL) {
        quote.total = Decimal::add(quote.total, quote_delta);
      }
      quote.available = Decimal::sub(quote.total, quote.reserved);
      quote.updated_at = std::chrono::system_clock::now();
    }

    Decimal fee_total = Decimal::zero();
    std::string fee_currency;
    if (report.has_fee_total()) {
      fee_total = Decimal::from_proto(report.fee_total().cost().amount());
      fee_currency = report.fee_total().cost().currency();
      if (fee_currency.empty()) fee_currency = quote_ccy;
    }
    Decimal fee_delta = Decimal::zero();
    if (Decimal::cmp(fee_total, state.fee_total) > 0) {
      fee_delta = Decimal::sub(fee_total, state.fee_total);
    }
    if (!fee_currency.empty() && Decimal::cmp(fee_delta, Decimal::zero()) > 0) {
      auto& fee_bal = ensure_venue_balance_locked(report.venue(), fee_currency);
      fee_bal.total = Decimal::sub(fee_bal.total, fee_delta);
      fee_bal.available = Decimal::sub(fee_bal.total, fee_bal.reserved);
      fee_bal.updated_at = std::chrono::system_clock::now();
    }

    HedgePnlRecord record;
    record.hedge_id = execution_report_key(report);
    record.venue = report.venue();
    record.instrument_symbol = intent.instrument().symbol();
    record.side = intent.side();
    record.executed_qty = delta_qty;
    record.executed_price = average_price;
    record.internal_price = internal_price;
    record.hedge_pnl = calculate_hedge_pnl(intent.side(), average_price, internal_price, delta_qty);
    record.timestamp = timestamp_to_time_point(report_timestamp(report));
    hedge_pnl_records_[record.venue].push_back(record);

    const std::string summary_key = record.venue + "|" + record.instrument_symbol;
    auto it = hedge_pnl_summary_.find(summary_key);
    if (it == hedge_pnl_summary_.end()) {
      hedge_pnl_summary_[summary_key] = record.hedge_pnl;
    } else {
      hedge_pnl_summary_[summary_key] = Decimal::add(it->second, record.hedge_pnl);
    }

    // F-12 / IN-009 DoD-6 (PR-F12-3c): accumulate hedge_pnl + fee delta
    // into PG `hedgeflows` row. Same hedge_flow_id ← intent_id fallback
    // as venues' PostgresHedgeflowRepository (PR-F12-3a) — matching's
    // external_fill reports carry composite intent_id, not UUID.
    if (hedgeflow_pnl_sink_) {
      const std::string hedge_flow_id =
          !report.hedge_flow_id().empty() ? report.hedge_flow_id() : report.intent_id();
      const std::string fee_delta_str =
          (report.has_fee_total() && report.fee_total().has_cost() &&
           report.fee_total().cost().has_amount())
              ? Decimal::from_proto(report.fee_total().cost().amount()).to_string()
              : std::string{};
      hedgeflow_pnl_sink_->UpdateHedgePnlDelta(
          hedge_flow_id,
          record.hedge_pnl.to_string(),
          fee_delta_str);
    }

    cex::common::log_json("INFO", "Hedge PnL computed (F-12 DoD-6)",
                          {{"hedge_flow_id", !report.hedge_flow_id().empty()
                                                 ? report.hedge_flow_id()
                                                 : report.intent_id()},
                           {"venue", report.venue()},
                           {"side", record.side == fob::common::v1::SIDE_BUY ? "BUY" : "SELL"},
                           {"delta_qty", record.executed_qty.to_string()},
                           {"executed_price", record.executed_price.to_string()},
                           {"internal_price", record.internal_price.to_string()},
                           {"hedge_pnl_delta", record.hedge_pnl.to_string()}});

    ++exec_recon_stats_.applied_reports;
  }

  state.status = status;
  state.filled_qty = filled_qty;
  state.remaining_qty = remaining_qty;
  state.average_price = average_price;
  if (report.has_fee_total()) {
    state.fee_total = Decimal::from_proto(report.fee_total().cost().amount());
    state.fee_currency = report.fee_total().cost().currency();
  }
  state.terminal = is_terminal_status(status);

  cex::common::log_json("INFO", "Execution report reconciled",
                        {{"intent_id", report.intent_id()},
                         {"venue", report.venue()},
                         {"status", std::to_string(status)},
                         {"filled_qty", filled_qty.to_string()},
                         {"remaining_qty", remaining_qty.to_string()}});
  return true;
}

fob::ledger::v1::RecordHedgeExecutionResponse LedgerUseCases::RecordHedgeExecution(
    const fob::ledger::v1::RecordHedgeExecutionRequest& req) {

  fob::ledger::v1::RecordHedgeExecutionResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  const auto& hedge = req.hedge();

  if (hedge.hedge_id().empty()) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("INVALID_HEDGE_ID");
    e->set_message("hedge_id cannot be empty");
    return resp;
  }

  if (hedge.venue().empty()) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("INVALID_VENUE");
    e->set_message("venue cannot be empty");
    return resp;
  }

  {
    std::lock_guard<std::mutex> lg(mu_);

    // Check for duplicate hedge_id (idempotency)
    for (const auto& existing : hedge_pnl_records_[hedge.venue()]) {
      if (existing.hedge_id == hedge.hedge_id()) {
        cex::common::log_json("INFO", "Hedge execution already recorded, skipping",
                              {{"hedge_id", hedge.hedge_id()}});
        resp.set_success(true);
        return resp;
      }
    }

    // Convert proto values to Decimal
    Decimal qty = Decimal::from_proto(hedge.executed_qty());
    Decimal exec_price = Decimal::from_proto(hedge.executed_price());
    Decimal internal_price = Decimal::from_proto(hedge.internal_price());

    // Calculate hedge PnL
    Decimal hedge_pnl = calculate_hedge_pnl(hedge.side(), exec_price, internal_price, qty);

    // Create record
    HedgePnlRecord record;
    record.hedge_id = hedge.hedge_id();
    record.venue = hedge.venue();
    record.instrument_symbol = hedge.instrument_symbol();
    record.side = hedge.side();
    record.executed_qty = qty;
    record.executed_price = exec_price;
    record.internal_price = internal_price;
    record.hedge_pnl = hedge_pnl;

    // Parse timestamp
    if (hedge.has_timestamp()) {
      record.timestamp = timestamp_to_time_point(hedge.timestamp());
    } else {
      record.timestamp = std::chrono::system_clock::now();
    }

    // Store record
    hedge_pnl_records_[record.venue].push_back(record);

    // Update summary (venue + instrument)
    std::string summary_key = record.venue + "|" + record.instrument_symbol;
    auto it = hedge_pnl_summary_.find(summary_key);
    if (it == hedge_pnl_summary_.end()) {
      hedge_pnl_summary_[summary_key] = hedge_pnl;
    } else {
      hedge_pnl_summary_[summary_key] = Decimal::add(it->second, hedge_pnl);
    }

    resp.set_success(true);

    cex::common::log_json("INFO", "Hedge execution recorded",
                          {{"hedge_id", hedge.hedge_id()},
                           {"venue", hedge.venue()},
                           {"instrument", hedge.instrument_symbol()},
                           {"side", hedge.side() == fob::common::v1::SIDE_BUY ? "BUY" : "SELL"},
                           {"qty", qty.to_string()},
                           {"exec_price", exec_price.to_string()},
                           {"internal_price", internal_price.to_string()},
                           {"hedge_pnl", hedge_pnl.to_string()}});
  }

  // Persist to repository (outside of lock)
  if (hedge_entries_repo_) {
    try {
      hedge_entries_repo_->CreateHedgeEntry(hedge);
    } catch (const std::exception& e) {
      cex::common::log_json("ERROR", "Failed to persist hedge entry",
                            {{"hedge_id", hedge.hedge_id()},
                             {"error", e.what()}});
    }
  }

  return resp;
}

fob::ledger::v1::GetHedgePnLResponse LedgerUseCases::GetHedgePnL(
    const fob::ledger::v1::GetHedgePnLRequest& req) {
  
  fob::ledger::v1::GetHedgePnLResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);
  
  // Parse time filters if present
  bool has_from = req.has_from_time();
  bool has_to = req.has_to_time();
  std::chrono::system_clock::time_point from_time;
  std::chrono::system_clock::time_point to_time;
  
  if (has_from) from_time = timestamp_to_time_point(req.from_time());
  if (has_to) to_time = timestamp_to_time_point(req.to_time());
  
  // Aggregate results
  std::unordered_map<std::string, fob::ledger::v1::HedgePnL> agg_map;
  
  for (const auto& [venue, records] : hedge_pnl_records_) {
    // Filter by venue
    if (!req.venue().empty() && venue != req.venue()) {
      continue;
    }
    
    for (const auto& record : records) {
      // Filter by instrument
      if (!req.instrument_symbol().empty() && record.instrument_symbol != req.instrument_symbol()) {
        continue;
      }
      
      // Filter by time range
      if (has_from && record.timestamp < from_time) continue;
      if (has_to && record.timestamp > to_time) continue;
      
      std::string key = venue + "|" + record.instrument_symbol;
      auto& agg = agg_map[key];
      
      if (agg.venue().empty()) {
        agg.set_venue(venue);
        agg.set_instrument_symbol(record.instrument_symbol);
        *agg.mutable_total_hedge_pnl() = record.hedge_pnl.to_proto();
        agg.set_hedge_count(1);
        *agg.mutable_total_hedge_volume() = record.executed_qty.to_proto();
      } else {
        Decimal existing_pnl = Decimal::from_proto(agg.total_hedge_pnl());
        Decimal new_pnl = Decimal::add(existing_pnl, record.hedge_pnl);
        *agg.mutable_total_hedge_pnl() = new_pnl.to_proto();
        agg.set_hedge_count(agg.hedge_count() + 1);
        
        Decimal existing_vol = Decimal::from_proto(agg.total_hedge_volume());
        Decimal new_vol = Decimal::add(existing_vol, record.executed_qty);
        *agg.mutable_total_hedge_volume() = new_vol.to_proto();
      }
    }
  }
  
  // Convert map to response
  for (auto& [key, agg] : agg_map) {
    (void)key;
    *resp.add_results() = agg;
  }
  
  cex::common::log_json("INFO", "GetHedgePnL",
                        {{"venue_filter", req.venue()},
                         {"instrument_filter", req.instrument_symbol()},
                         {"results_count", std::to_string(resp.results_size())}});
  
  return resp;
}

}  // namespace cex::ledger::app
