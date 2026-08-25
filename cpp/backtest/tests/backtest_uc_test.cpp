#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "app/backtest_uc.hpp"

namespace {

struct FakeReplayStorage final : public cex::backtest::app::IBatchReplayStorage {
  int save_batch_calls{0};
  int save_fills_calls{0};
  std::string last_batch_id;
  bool fail_batch{false};
  bool fail_fills{false};

  bool SaveBatchResult(const fob::matching::v1::BatchResult& evt) override {
    ++save_batch_calls;
    last_batch_id = evt.batch_id();
    return !fail_batch;
  }

  bool SaveFills(const fob::matching::v1::BatchResult& evt) override {
    ++save_fills_calls;
    last_batch_id = evt.batch_id();
    return !fail_fills;
  }
};

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool test_batch_result_persisted() {
  FakeReplayStorage storage;
  cex::backtest::app::BacktestUseCases uc(&storage);

  fob::matching::v1::BatchResult batch;
  batch.set_batch_id("batch-1");
  auto* fill1 = batch.add_fills();
  fill1->set_order_id("order-1");
  auto* fill2 = batch.add_fills();
  fill2->set_order_id("order-2");

  uc.OnBatchResult(batch);

  if (!Check(storage.save_batch_calls == 1, "SaveBatchResult must be called once")) return false;
  if (!Check(storage.save_fills_calls == 1, "SaveFills must be called once")) return false;
  if (!Check(storage.last_batch_id == "batch-1", "batch_id must be passed to storage")) return false;

  auto stats = uc.GetStats();
  if (!Check(stats.batches_received == 1, "batches_received must be 1")) return false;
  if (!Check(stats.batches_saved == 1, "batches_saved must be 1")) return false;
  if (!Check(stats.fills_saved == 2, "fills_saved must equal number of fills")) return false;
  if (!Check(stats.last_batch_id == "batch-1", "last_batch_id must be updated")) return false;

  return true;
}

bool test_multiple_batches() {
  FakeReplayStorage storage;
  cex::backtest::app::BacktestUseCases uc(&storage);

  for (int i = 1; i <= 3; ++i) {
    fob::matching::v1::BatchResult batch;
    batch.set_batch_id("batch-" + std::to_string(i));
    batch.add_fills()->set_order_id("order-" + std::to_string(i));
    uc.OnBatchResult(batch);
  }

  if (!Check(storage.save_batch_calls == 3, "SaveBatchResult must be called 3 times")) return false;
  if (!Check(storage.save_fills_calls == 3, "SaveFills must be called 3 times")) return false;

  auto stats = uc.GetStats();
  if (!Check(stats.batches_received == 3, "batches_received must be 3")) return false;
  if (!Check(stats.batches_saved == 3, "batches_saved must be 3")) return false;
  if (!Check(stats.fills_saved == 3, "fills_saved must be 3")) return false;
  if (!Check(stats.last_batch_id == "batch-3", "last_batch_id must be batch-3")) return false;

  return true;
}

bool test_no_storage_configured() {
  cex::backtest::app::BacktestUseCases uc(nullptr);

  fob::matching::v1::BatchResult batch;
  batch.set_batch_id("batch-no-storage");
  batch.add_fills()->set_order_id("order-1");
  uc.OnBatchResult(batch);

  auto stats = uc.GetStats();
  if (!Check(stats.batches_received == 1, "batches_received must be 1 even without storage"))
    return false;
  if (!Check(stats.batches_saved == 0, "batches_saved must be 0 without storage")) return false;
  if (!Check(stats.fills_saved == 0, "fills_saved must be 0 without storage")) return false;

  return true;
}

bool test_storage_failure_does_not_break() {
  FakeReplayStorage storage;
  storage.fail_batch = true;
  cex::backtest::app::BacktestUseCases uc(&storage);

  fob::matching::v1::BatchResult batch;
  batch.set_batch_id("batch-fail");
  batch.add_fills()->set_order_id("order-1");
  uc.OnBatchResult(batch);

  auto stats = uc.GetStats();
  if (!Check(stats.batches_received == 1, "batches_received must be 1")) return false;
  if (!Check(stats.batches_saved == 0, "batches_saved must be 0 on failure")) return false;
  if (!Check(stats.fills_saved == 1, "fills_saved must be 1 (fills succeeded)")) return false;

  return true;
}

bool test_empty_batch() {
  FakeReplayStorage storage;
  cex::backtest::app::BacktestUseCases uc(&storage);

  fob::matching::v1::BatchResult batch;
  batch.set_batch_id("batch-empty");
  uc.OnBatchResult(batch);

  if (!Check(storage.save_batch_calls == 1, "SaveBatchResult must be called for empty batch"))
    return false;
  if (!Check(storage.save_fills_calls == 1, "SaveFills must be called for empty batch"))
    return false;

  auto stats = uc.GetStats();
  if (!Check(stats.batches_saved == 1, "batches_saved must be 1")) return false;
  if (!Check(stats.fills_saved == 0, "fills_saved must be 0 for empty batch")) return false;

  return true;
}

}  // namespace

int main() {
  bool all_passed = true;

  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) {
      std::cerr << "  in test: " << name << std::endl;
      all_passed = false;
    }
  };

  run("test_batch_result_persisted", test_batch_result_persisted);
  run("test_multiple_batches", test_multiple_batches);
  run("test_no_storage_configured", test_no_storage_configured);
  run("test_storage_failure_does_not_break", test_storage_failure_does_not_break);
  run("test_empty_batch", test_empty_batch);

  if (all_passed) {
    std::cout << "[OK] backtest_uc_test passed (5 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
