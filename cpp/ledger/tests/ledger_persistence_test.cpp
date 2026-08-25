#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>

#include "app/ledger_uc.hpp"
#include "app/persistence_ports.hpp"

namespace {

class FakePositionsRepository final : public cex::ledger::app::PositionsRepositoryPort {
 public:
  explicit FakePositionsRepository(bool fail_on_apply = false)
      : fail_on_apply_(fail_on_apply) {}

  void ApplyFill(const fob::matching::v1::FlowFill& fill) override {
    ++apply_fill_calls;
    last_order_id = fill.order_id();
    if (fail_on_apply_) throw std::runtime_error("positions repo failure");
  }

  int apply_fill_calls{0};
  std::string last_order_id;

 private:
  bool fail_on_apply_{false};
};

class FakeLedgerEntriesRepository final : public cex::ledger::app::LedgerEntriesRepositoryPort {
 public:
  explicit FakeLedgerEntriesRepository(bool fail_on_create = false)
      : fail_on_create_(fail_on_create) {}

  void CreateEntriesForFill(const std::string& batch_id,
                            const fob::matching::v1::FlowFill& fill) override {
    ++create_entries_calls;
    last_batch_id = batch_id;
    last_order_id = fill.order_id();
    if (fail_on_create_) throw std::runtime_error("entries repo failure");
  }

  int create_entries_calls{0};
  std::string last_batch_id;
  std::string last_order_id;

 private:
  bool fail_on_create_{false};
};

fob::matching::v1::FlowFill make_fill(const std::string& order_id,
                                      fob::common::v1::Side side,
                                      int64_t qty_units,
                                      int32_t qty_scale,
                                      int64_t notional_units,
                                      int32_t notional_scale) {
  fob::matching::v1::FlowFill fill;
  fill.set_order_id(order_id);
  fill.set_user_id("demo-user");
  fill.mutable_instrument()->set_symbol("BTC/USDT");
  fill.mutable_instrument()->set_base("BTC");
  fill.mutable_instrument()->set_quote("USDT");
  fill.set_side(side);
  fill.mutable_executed_qty()->set_units(qty_units);
  fill.mutable_executed_qty()->set_scale(qty_scale);
  fill.mutable_executed_notional()->set_units(notional_units);
  fill.mutable_executed_notional()->set_scale(notional_scale);
  return fill;
}

void test_persistence_ports_are_called_for_each_fill() {
  auto positions_repo = std::make_shared<FakePositionsRepository>();
  auto entries_repo = std::make_shared<FakeLedgerEntriesRepository>();
  cex::ledger::app::LedgerUseCases uc(positions_repo, entries_repo);

  fob::ledger::v1::ApplyBatchResultRequest req;
  req.mutable_batch()->set_batch_id("batch-persist-1");
  *req.mutable_batch()->add_fills() =
      make_fill("o-buy", fob::common::v1::SIDE_BUY, 1000000, 8, 5000, 2);
  *req.mutable_batch()->add_fills() =
      make_fill("o-sell", fob::common::v1::SIDE_SELL, 2000000, 8, 10000, 2);

  auto resp = uc.ApplyBatchResult(req);
  assert(resp.success());

  assert(positions_repo->apply_fill_calls == 2);
  assert(entries_repo->create_entries_calls == 2);
  assert(entries_repo->last_batch_id == "batch-persist-1");
  assert(entries_repo->last_order_id == "o-sell");
}

void test_persistence_errors_do_not_break_batch_application() {
  auto positions_repo = std::make_shared<FakePositionsRepository>(true);
  auto entries_repo = std::make_shared<FakeLedgerEntriesRepository>(true);
  cex::ledger::app::LedgerUseCases uc(positions_repo, entries_repo);

  fob::ledger::v1::ApplyBatchResultRequest req;
  req.mutable_batch()->set_batch_id("batch-persist-err");
  *req.mutable_batch()->add_fills() =
      make_fill("o-err", fob::common::v1::SIDE_BUY, 1000000, 8, 5000, 2);

  auto resp = uc.ApplyBatchResult(req);
  assert(resp.success());
  assert(positions_repo->apply_fill_calls == 1);
  assert(entries_repo->create_entries_calls == 1);
}

}  // namespace

int main() {
  test_persistence_ports_are_called_for_each_fill();
  test_persistence_errors_do_not_break_batch_application();
  return 0;
}
