#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

#include "app/ledger_uc.hpp"
#include "app/persistence_ports.hpp"

namespace {

class FakeIdempotencyRepository final : public cex::ledger::app::IdempotencyRepositoryPort {
 public:
  bool IsBatchProcessed(const std::string& batch_id) override {
    ++is_batch_processed_calls;
    return processed_batches.contains(batch_id);
  }

  void MarkBatchProcessed(const std::string& batch_id) override {
    ++mark_batch_processed_calls;
    processed_batches.insert(batch_id);
  }

  int is_batch_processed_calls{0};
  int mark_batch_processed_calls{0};

 private:
  std::unordered_set<std::string> processed_batches;
};

fob::common::v1::Decimal make_decimal(const int64_t units, const int32_t scale) {
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
  req.set_user_id(user_id);
  return uc.GetBalances(req);
}

void reserve_usdt_for_buy(
    cex::ledger::app::LedgerUseCases& uc,
    const std::string& reservation_id,
    const int64_t units) {
  fob::ledger::v1::ReserveFundsRequest req;
  req.set_reservation_id(reservation_id);
  req.set_user_id("demo-user");
  req.set_order_id("order-buy-1");
  req.set_currency("USDT");
  *req.mutable_amount() = make_decimal(units, 2);
  req.set_reason(fob::ledger::v1::RESERVE_REASON_NEW_ORDER);

  const auto resp = uc.ReserveFunds(req);
  assert(resp.success());
}

fob::ledger::v1::ApplyBatchResultRequest make_apply_request(const std::string& batch_id) {
  fob::ledger::v1::ApplyBatchResultRequest req;
  req.mutable_batch()->set_batch_id(batch_id);

  auto* fill = req.mutable_batch()->add_fills();
  fill->set_order_id("order-buy-1");
  fill->set_user_id("demo-user");
  fill->mutable_instrument()->set_symbol("BTC/USDT");
  fill->mutable_instrument()->set_base("BTC");
  fill->mutable_instrument()->set_quote("USDT");
  fill->set_side(fob::common::v1::SIDE_BUY);
  *fill->mutable_executed_qty() = make_decimal(1'000'000, 8);       // 0.01 BTC
  *fill->mutable_executed_notional() = make_decimal(5'000, 2);       // 50.00 USDT

  return req;
}

void test_repeated_batch_is_processed_once() {
  cex::ledger::app::LedgerUseCases uc;
  auto idempotency_repo = std::make_shared<FakeIdempotencyRepository>();
  uc.SetIdempotencyRepo(idempotency_repo);

  reserve_usdt_for_buy(uc, "res-idem-1", 10'000);  // 100.00 USDT reserved

  const auto first_resp = uc.ApplyBatchResult(make_apply_request("batch-idem-1"));
  assert(first_resp.success());

  const auto after_first = get_balances(uc, "demo-user");
  const auto* usdt_after_first = find_balance(after_first, "USDT");
  const auto* btc_after_first = find_balance(after_first, "BTC");
  assert(usdt_after_first != nullptr);
  assert(btc_after_first != nullptr);
  assert(usdt_after_first->available().units() == 990'000);
  assert(usdt_after_first->reserved().units() == 5'000);
  assert(btc_after_first->available().units() == 251'000'000);

  const auto second_resp = uc.ApplyBatchResult(make_apply_request("batch-idem-1"));
  assert(second_resp.success());

  const auto after_second = get_balances(uc, "demo-user");
  const auto* usdt_after_second = find_balance(after_second, "USDT");
  const auto* btc_after_second = find_balance(after_second, "BTC");
  assert(usdt_after_second != nullptr);
  assert(btc_after_second != nullptr);
  assert(usdt_after_second->available().units() == usdt_after_first->available().units());
  assert(usdt_after_second->reserved().units() == usdt_after_first->reserved().units());
  assert(btc_after_second->available().units() == btc_after_first->available().units());

  assert(idempotency_repo->is_batch_processed_calls == 2);
  assert(idempotency_repo->mark_batch_processed_calls == 1);
}

}  // namespace

int main() {
  test_repeated_batch_is_processed_once();
  return 0;
}
