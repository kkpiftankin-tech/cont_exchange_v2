// ============================================================================
// ledger_positions_update_test.cpp — F-06 / F6-5 (T-F06-072, ADR-046).
//
// Проверяет, что после успешного ApplyBatchResult ledger публикует сигнал
// positions.update по каждому УНИКАЛЬНОМУ user_id из fills (дедуп в рамках
// батча), через fake-publisher (без реального Kafka).
//
//   P1  батч с fills двух пользователей → Publish вызван 2 раза,
//       по одному уникальному user_id, с правильным batch_id.
//   P2  дублирующиеся fills одного юзера → один Publish на user_id.
//   P3  publisher не задан (nullptr) → ApplyBatchResult не падает.
// ============================================================================
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "app/ledger_uc.hpp"
#include "app/persistence_ports.hpp"

using cex::ledger::app::LedgerUseCases;
using cex::ledger::app::PositionsUpdatePublisherPort;

namespace {

struct Call {
  std::string user_id;
  std::string batch_id;
  int64_t ts_unix{0};
};

// Fake publisher — записывает все вызовы Publish вместо отправки в Kafka.
class FakePositionsUpdatePublisher : public PositionsUpdatePublisherPort {
 public:
  bool Publish(const std::string& user_id, const std::string& batch_id,
               int64_t ts_unix) override {
    calls.push_back(Call{user_id, batch_id, ts_unix});
    return true;
  }
  std::vector<Call> calls;
};

fob::common::v1::Decimal dec(int64_t units, int32_t scale) {
  fob::common::v1::Decimal d;
  d.set_units(units);
  d.set_scale(scale);
  return d;
}

fob::matching::v1::FlowFill make_fill(const std::string& order_id,
                                      const std::string& user,
                                      fob::common::v1::Side side) {
  fob::matching::v1::FlowFill f;
  f.set_order_id(order_id);
  f.set_user_id(user);
  f.mutable_instrument()->set_symbol("BTC/USDT");
  f.mutable_instrument()->set_base("BTC");
  f.mutable_instrument()->set_quote("USDT");
  f.set_side(side);
  *f.mutable_executed_qty() = dec(1'00000000, 8);   // 1 BTC
  *f.mutable_price() = dec(500000, 2);              // 5000.00
  *f.mutable_executed_notional() = dec(500000, 2);  // 5000.00
  return f;
}

bool contains_user(const std::vector<Call>& calls, const std::string& user) {
  return std::any_of(calls.begin(), calls.end(),
                     [&](const Call& c) { return c.user_id == user; });
}

// P1: два разных пользователя в одном батче → 2 Publish с уникальными user_id.
void test_two_users_two_publishes() {
  LedgerUseCases uc;
  auto pub = std::make_shared<FakePositionsUpdatePublisher>();
  uc.SetPositionsUpdatePublisher(pub);

  fob::ledger::v1::ApplyBatchResultRequest req;
  req.mutable_batch()->set_batch_id("batch-1");
  *req.mutable_batch()->add_fills() =
      make_fill("o1", "alice", fob::common::v1::SIDE_BUY);
  *req.mutable_batch()->add_fills() =
      make_fill("o2", "bob", fob::common::v1::SIDE_SELL);

  auto resp = uc.ApplyBatchResult(req);
  assert(resp.success());

  assert(pub->calls.size() == 2);
  assert(contains_user(pub->calls, "alice"));
  assert(contains_user(pub->calls, "bob"));
  for (const auto& c : pub->calls) {
    assert(c.batch_id == "batch-1");
  }
  std::cout << "P1 two users -> 2 unique publishes: PASSED\n";
}

// P2: несколько fills одного пользователя → дедуп до одного Publish на user.
void test_dedup_same_user() {
  LedgerUseCases uc;
  auto pub = std::make_shared<FakePositionsUpdatePublisher>();
  uc.SetPositionsUpdatePublisher(pub);

  fob::ledger::v1::ApplyBatchResultRequest req;
  req.mutable_batch()->set_batch_id("batch-2");
  *req.mutable_batch()->add_fills() =
      make_fill("o1", "alice", fob::common::v1::SIDE_BUY);
  *req.mutable_batch()->add_fills() =
      make_fill("o2", "alice", fob::common::v1::SIDE_BUY);
  *req.mutable_batch()->add_fills() =
      make_fill("o3", "carol", fob::common::v1::SIDE_BUY);

  auto resp = uc.ApplyBatchResult(req);
  assert(resp.success());

  // alice дедуплицирована → 2 уникальных user_id (alice, carol).
  assert(pub->calls.size() == 2);
  assert(contains_user(pub->calls, "alice"));
  assert(contains_user(pub->calls, "carol"));
  std::cout << "P2 dedup same user -> 1 publish per user: PASSED\n";
}

// P3: publisher не задан → ApplyBatchResult не падает, success=true.
void test_no_publisher_ok() {
  LedgerUseCases uc;  // publisher не установлен
  fob::ledger::v1::ApplyBatchResultRequest req;
  req.mutable_batch()->set_batch_id("batch-3");
  *req.mutable_batch()->add_fills() =
      make_fill("o1", "alice", fob::common::v1::SIDE_BUY);

  auto resp = uc.ApplyBatchResult(req);
  assert(resp.success());
  std::cout << "P3 no publisher -> ApplyBatchResult OK: PASSED\n";
}

}  // namespace

int main() {
  test_two_users_two_publishes();
  test_dedup_same_user();
  test_no_publisher_ok();
  std::cout << "All positions.update publisher tests passed!\n";
  return 0;
}
