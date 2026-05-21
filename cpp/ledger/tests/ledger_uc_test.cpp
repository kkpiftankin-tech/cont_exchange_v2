#include <cassert>
#include <string>

#include "app/ledger_uc.hpp"

namespace {

fob::common::v1::EventMeta make_meta(const std::string& correlation_id) {
  fob::common::v1::EventMeta meta;
  meta.set_event_id("test-event");
  meta.set_source("ledger-test");
  meta.set_correlation_id(correlation_id);
  return meta;
}

fob::common::v1::Decimal make_decimal(int64_t units, int32_t scale) {
  fob::common::v1::Decimal d;
  d.set_units(units);
  d.set_scale(scale);
  return d;
}

const fob::ledger::v1::Balance* find_balance(
    const fob::ledger::v1::GetBalancesResponse& resp,
    const std::string& currency) {
  for (const auto& b : resp.balances()) {
    if (b.currency() == currency) return &b;
  }
  return nullptr;
}

fob::ledger::v1::GetBalancesResponse get_balances(
    cex::ledger::app::LedgerUseCases& uc,
    const std::string& user_id) {
  fob::ledger::v1::GetBalancesRequest req;
  *req.mutable_meta() = make_meta("get-balances");
  req.set_user_id(user_id);
  return uc.GetBalances(req);
}

void reserve_funds(
    cex::ledger::app::LedgerUseCases& uc,
    const std::string& reservation_id,
    const std::string& order_id,
    const std::string& currency,
    int64_t amount_units,
    int32_t amount_scale) {
  fob::ledger::v1::ReserveFundsRequest req;
  *req.mutable_meta() = make_meta("reserve-" + reservation_id);
  req.set_reservation_id(reservation_id);
  req.set_user_id("demo-user");
  req.set_order_id(order_id);
  req.set_currency(currency);
  *req.mutable_amount() = make_decimal(amount_units, amount_scale);
  req.set_reason(fob::ledger::v1::RESERVE_REASON_NEW_ORDER);

  auto resp = uc.ReserveFunds(req);
  assert(resp.success());
}

void test_apply_batch_result_buy_updates_balances() {
  cex::ledger::app::LedgerUseCases uc;

  // Reserve 100.00 USDT for a BUY order.
  reserve_funds(uc, "res-buy-1", "order-buy-1", "USDT", 10'000, 2);

  fob::ledger::v1::ApplyBatchResultRequest req;
  *req.mutable_meta() = make_meta("apply-buy");
  req.mutable_batch()->set_batch_id("batch-buy-1");

  auto* fill = req.mutable_batch()->add_fills();
  fill->set_order_id("order-buy-1");
  fill->set_user_id("demo-user");
  fill->mutable_instrument()->set_symbol("BTC/USDT");
  fill->mutable_instrument()->set_base("BTC");
  fill->mutable_instrument()->set_quote("USDT");
  fill->set_side(fob::common::v1::SIDE_BUY);
  *fill->mutable_executed_qty() = make_decimal(1'000'000, 8);       // 0.01 BTC
  *fill->mutable_executed_notional() = make_decimal(5'000, 2);       // 50.00 USDT

  auto apply_resp = uc.ApplyBatchResult(req);
  assert(apply_resp.success());

  auto balances = get_balances(uc, "demo-user");
  const auto* usdt = find_balance(balances, "USDT");
  const auto* btc = find_balance(balances, "BTC");
  assert(usdt != nullptr);
  assert(btc != nullptr);

  // USDT: 10000.00 -> reserve 100.00 => available 9900.00, reserved 100.00
  // apply fill notional 50.00 => reserved 50.00, available unchanged.
  assert(usdt->available().units() == 990'000);
  assert(usdt->available().scale() == 2);
  assert(usdt->reserved().units() == 5'000);
  assert(usdt->reserved().scale() == 2);

  // BTC seed 2.50000000 + 0.01000000 = 2.51000000
  assert(btc->available().units() == 251'000'000);
  assert(btc->available().scale() == 8);
  assert(btc->reserved().units() == 0);
  assert(btc->reserved().scale() == 8);
}

void test_apply_batch_result_sell_updates_balances() {
  cex::ledger::app::LedgerUseCases uc;

  // Reserve 0.10 BTC for a SELL order.
  reserve_funds(uc, "res-sell-1", "order-sell-1", "BTC", 10'000'000, 8);

  fob::ledger::v1::ApplyBatchResultRequest req;
  *req.mutable_meta() = make_meta("apply-sell");
  req.mutable_batch()->set_batch_id("batch-sell-1");

  auto* fill = req.mutable_batch()->add_fills();
  fill->set_order_id("order-sell-1");
  fill->set_user_id("demo-user");
  fill->mutable_instrument()->set_symbol("BTC/USDT");
  fill->mutable_instrument()->set_base("BTC");
  fill->mutable_instrument()->set_quote("USDT");
  fill->set_side(fob::common::v1::SIDE_SELL);
  *fill->mutable_executed_qty() = make_decimal(4'000'000, 8);        // 0.04 BTC
  *fill->mutable_executed_notional() = make_decimal(8'000, 2);        // 80.00 USDT

  auto apply_resp = uc.ApplyBatchResult(req);
  assert(apply_resp.success());

  auto balances = get_balances(uc, "demo-user");
  const auto* usdt = find_balance(balances, "USDT");
  const auto* btc = find_balance(balances, "BTC");
  assert(usdt != nullptr);
  assert(btc != nullptr);

  // BTC seed 2.50000000 -> reserve 0.10000000 => available 2.40000000, reserved 0.10000000
  // apply fill qty 0.04000000 => reserved 0.06000000, available unchanged.
  assert(btc->available().units() == 240'000'000);
  assert(btc->available().scale() == 8);
  assert(btc->reserved().units() == 6'000'000);
  assert(btc->reserved().scale() == 8);

  // USDT: 10000.00 + 80.00 = 10080.00
  assert(usdt->available().units() == 1'008'000);
  assert(usdt->available().scale() == 2);
  assert(usdt->reserved().units() == 0);
  assert(usdt->reserved().scale() == 2);
}

void test_apply_batch_result_empty_batch_keeps_balances() {
  cex::ledger::app::LedgerUseCases uc;

  auto before = get_balances(uc, "demo-user");

  fob::ledger::v1::ApplyBatchResultRequest req;
  *req.mutable_meta() = make_meta("apply-empty");
  req.mutable_batch()->set_batch_id("batch-empty");
  auto apply_resp = uc.ApplyBatchResult(req);
  assert(apply_resp.success());

  auto after = get_balances(uc, "demo-user");

  const auto* before_usdt = find_balance(before, "USDT");
  const auto* before_btc = find_balance(before, "BTC");
  const auto* after_usdt = find_balance(after, "USDT");
  const auto* after_btc = find_balance(after, "BTC");
  assert(before_usdt != nullptr && before_btc != nullptr);
  assert(after_usdt != nullptr && after_btc != nullptr);

  assert(after_usdt->available().units() == before_usdt->available().units());
  assert(after_usdt->reserved().units() == before_usdt->reserved().units());
  assert(after_btc->available().units() == before_btc->available().units());
  assert(after_btc->reserved().units() == before_btc->reserved().units());
}

}  // namespace

int main() {
  test_apply_batch_result_buy_updates_balances();
  test_apply_batch_result_sell_updates_balances();
  test_apply_batch_result_empty_batch_keeps_balances();
  return 0;
}
