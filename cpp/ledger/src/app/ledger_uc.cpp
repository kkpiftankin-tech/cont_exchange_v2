#include "app/ledger_uc.hpp"

#include "cex/common/log.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

namespace cex::ledger::app {

using cex::common::Decimal;

LedgerUseCases::LedgerUseCases() {
  // Demo initial balances so you can place orders immediately.
  // user "demo-user" has 10k USDT and 1 BTC.
  std::lock_guard<std::mutex> lg(mu_);

  balances_["demo-user"]["USDT"].available = Decimal{10'00000, 2}; // 10000.00 USDT
  balances_["demo-user"]["USDT"].reserved  = Decimal{0, 2};

  balances_["demo-user"]["BTC"].available = Decimal{1'00000000, 8}; // 1.0 BTC
  balances_["demo-user"]["BTC"].reserved  = Decimal{0, 8};
}

LedgerUseCases::Balance& LedgerUseCases::ensure_balance_locked(
    const std::string& user, const std::string& currency) {
  // Ensure both available/reserved exist with the same scale as existing values.
  auto& ub = balances_[user];
  auto& b = ub[currency];
  return b;
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

    if (fill.side() == fob::common::v1::SIDE_BUY) {
      // Decrease reserved quote by executed notional.
      auto& qbal = ensure_balance_locked(user, quote);
      qbal.reserved = Decimal::sub(qbal.reserved, notional);

      // Increase available base by executed qty.
      auto& bbal = ensure_balance_locked(user, base);
      bbal.available = Decimal::add(bbal.available, qty);
    } else if (fill.side() == fob::common::v1::SIDE_SELL) {
      // Decrease reserved base by executed qty.
      auto& bbal = ensure_balance_locked(user, base);
      bbal.reserved = Decimal::sub(bbal.reserved, qty);

      // Increase available quote by executed notional.
      auto& qbal = ensure_balance_locked(user, quote);
      qbal.available = Decimal::add(qbal.available, notional);
    }
  }

  resp.set_success(true);
  return resp;
}

void LedgerUseCases::ApplyExecutionReport(const fob::ledger::v1::ApplyExecutionReportRequest& req) {
  // MVP: hedge accounting not implemented.
  cex::common::log_json("INFO", "ApplyExecutionReport",
                        {{"venue", req.report().venue()},
                         {"intent_id", req.report().intent_id()},
                         {"status", std::to_string(req.report().status())}});
}

}  // namespace cex::ledger::app
