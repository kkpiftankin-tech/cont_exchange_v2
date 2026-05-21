#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "app/historical_batch_loader_port.hpp"
#include "app/historical_batch_loader_uc.hpp"

namespace {

using cex::backtest::app::HistoricalBatch;
using cex::backtest::app::HistoricalBatchLoaderUseCases;
using cex::backtest::app::HistoricalBatchResultRow;
using cex::backtest::app::HistoricalFillRow;
using cex::backtest::app::HistoricalMarketdataSnapshotRow;
using cex::backtest::app::IHistoricalBatchLoader;

struct FakeLoader final : public IHistoricalBatchLoader {
  std::vector<HistoricalBatchResultRow> batches;
  std::vector<HistoricalFillRow> fills;
  std::vector<HistoricalMarketdataSnapshotRow> snapshots;

  int batch_calls{0};
  int fill_calls{0};
  int snapshot_calls{0};

  // Last requested chunk window for each call (track all).
  std::vector<int64_t> batch_offsets;
  std::vector<int64_t> batch_limits;

  static std::vector<HistoricalBatchResultRow> SliceBatches(
      const std::vector<HistoricalBatchResultRow>& src,
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) {
    std::vector<HistoricalBatchResultRow> filtered;
    for (const auto& r : src) {
      if (r.event_time_ms >= from_ms && r.event_time_ms <= to_ms) filtered.push_back(r);
    }
    std::vector<HistoricalBatchResultRow> out;
    for (int64_t i = offset; i < static_cast<int64_t>(filtered.size()) && i < offset + limit; ++i) {
      out.push_back(filtered[i]);
    }
    return out;
  }

  std::vector<HistoricalBatchResultRow> LoadBatchResults(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    ++batch_calls;
    batch_offsets.push_back(offset);
    batch_limits.push_back(limit);
    return SliceBatches(batches, from_ms, to_ms, offset, limit);
  }

  std::vector<HistoricalFillRow> LoadFills(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    ++fill_calls;
    std::vector<HistoricalFillRow> filtered;
    for (const auto& r : fills) {
      if (r.event_time_ms >= from_ms && r.event_time_ms <= to_ms) filtered.push_back(r);
    }
    std::vector<HistoricalFillRow> out;
    for (int64_t i = offset; i < static_cast<int64_t>(filtered.size()) && i < offset + limit; ++i) {
      out.push_back(filtered[i]);
    }
    return out;
  }

  std::vector<HistoricalMarketdataSnapshotRow> LoadMarketdataSnapshots(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    ++snapshot_calls;
    std::vector<HistoricalMarketdataSnapshotRow> filtered;
    for (const auto& r : snapshots) {
      if (r.event_time_ms >= from_ms && r.event_time_ms <= to_ms) filtered.push_back(r);
    }
    std::vector<HistoricalMarketdataSnapshotRow> out;
    for (int64_t i = offset; i < static_cast<int64_t>(filtered.size()) && i < offset + limit; ++i) {
      out.push_back(filtered[i]);
    }
    return out;
  }
};

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

HistoricalBatchResultRow MakeBatch(const std::string& id, int64_t ts) {
  HistoricalBatchResultRow r;
  r.batch_id = id;
  r.event_time_ms = ts;
  r.fills_count = 0;
  return r;
}

HistoricalFillRow MakeFill(const std::string& batch_id,
                           const std::string& order_id,
                           int64_t ts,
                           double qty) {
  HistoricalFillRow f;
  f.batch_id = batch_id;
  f.order_id = order_id;
  f.event_time_ms = ts;
  f.executed_qty = qty;
  return f;
}

HistoricalMarketdataSnapshotRow MakeSnapshot(int64_t ts,
                                             const std::string& venue,
                                             const std::string& symbol) {
  HistoricalMarketdataSnapshotRow s;
  s.venue_id = venue;
  s.symbol = symbol;
  s.event_time_ms = ts;
  s.best_bid = 100.0;
  s.best_ask = 101.0;
  s.mid_price = 100.5;
  return s;
}

bool test_deterministic_order() {
  FakeLoader loader;
  // Insert intentionally out of order; loader returns as-is, UC must sort.
  loader.batches = {MakeBatch("b3", 200), MakeBatch("b1", 100), MakeBatch("b2", 100)};

  HistoricalBatchLoaderUseCases uc(&loader);
  HistoricalBatchLoaderUseCases::Config cfg;
  cfg.from_ms = 0;
  cfg.to_ms = 1000;
  cfg.chunk_size = 10;

  auto result = uc.Run(cfg);
  if (!Check(result.size() == 3, "must load all three batches")) return false;
  if (!Check(result[0].batch_result.batch_id == "b1",
             "deterministic order: ts=100,batch_id=b1 first")) return false;
  if (!Check(result[1].batch_result.batch_id == "b2",
             "deterministic order: ts=100,batch_id=b2 second")) return false;
  if (!Check(result[2].batch_result.batch_id == "b3",
             "deterministic order: ts=200 last")) return false;
  return true;
}

bool test_chunked_loading() {
  FakeLoader loader;
  for (int i = 0; i < 25; ++i) {
    loader.batches.push_back(
        MakeBatch("b" + std::to_string(i), static_cast<int64_t>(i)));
  }

  HistoricalBatchLoaderUseCases uc(&loader);
  HistoricalBatchLoaderUseCases::Config cfg;
  cfg.from_ms = 0;
  cfg.to_ms = 1000;
  cfg.chunk_size = 10;

  auto result = uc.Run(cfg);
  if (!Check(result.size() == 25, "must load 25 batches across chunks")) return false;
  // 25 rows / chunk_size 10 -> chunks: 10, 10, 5 (last < chunk_size, stop).
  if (!Check(loader.batch_calls == 3, "must paginate in 3 chunks")) return false;
  if (!Check(loader.batch_offsets == std::vector<int64_t>{0, 10, 20},
             "offsets must advance by chunk_size")) return false;
  return true;
}

bool test_chunk_size_exact_multiple_terminates() {
  FakeLoader loader;
  for (int i = 0; i < 20; ++i) {
    loader.batches.push_back(
        MakeBatch("b" + std::to_string(i), static_cast<int64_t>(i)));
  }
  HistoricalBatchLoaderUseCases uc(&loader);
  HistoricalBatchLoaderUseCases::Config cfg;
  cfg.from_ms = 0;
  cfg.to_ms = 1000;
  cfg.chunk_size = 10;

  auto result = uc.Run(cfg);
  if (!Check(result.size() == 20, "20 rows loaded")) return false;
  // Final empty chunk signals end: 10 + 10 + 0 -> 3 calls.
  if (!Check(loader.batch_calls == 3, "must perform one extra empty chunk read")) return false;
  return true;
}

bool test_fills_join_by_batch_id() {
  FakeLoader loader;
  loader.batches = {MakeBatch("b1", 100), MakeBatch("b2", 200)};
  loader.fills = {
      MakeFill("b1", "o-a", 100, 1.0),
      MakeFill("b2", "o-c", 200, 3.0),
      MakeFill("b1", "o-b", 100, 2.0),
  };

  HistoricalBatchLoaderUseCases uc(&loader);
  HistoricalBatchLoaderUseCases::Config cfg;
  cfg.from_ms = 0;
  cfg.to_ms = 1000;
  cfg.chunk_size = 100;
  auto result = uc.Run(cfg);

  if (!Check(result.size() == 2, "two batches loaded")) return false;
  if (!Check(result[0].batch_result.batch_id == "b1", "first is b1")) return false;
  if (!Check(result[0].fills.size() == 2, "b1 has two fills")) return false;
  // Fills sorted by order_id within batch.
  if (!Check(result[0].fills[0].order_id == "o-a", "fill order_id sorted: o-a first")) return false;
  if (!Check(result[0].fills[1].order_id == "o-b", "fill order_id sorted: o-b second")) return false;
  if (!Check(result[1].fills.size() == 1, "b2 has one fill")) return false;
  return true;
}

bool test_marketdata_join_by_timestamp() {
  FakeLoader loader;
  loader.batches = {MakeBatch("b1", 100), MakeBatch("b2", 250)};
  loader.snapshots = {
      MakeSnapshot(100, "binance", "BTC/USDT"),
      MakeSnapshot(100, "okx", "BTC/USDT"),
      MakeSnapshot(999, "binance", "BTC/USDT"),
  };

  HistoricalBatchLoaderUseCases uc(&loader);
  HistoricalBatchLoaderUseCases::Config cfg;
  cfg.from_ms = 0;
  cfg.to_ms = 1000;
  cfg.chunk_size = 100;
  auto result = uc.Run(cfg);

  if (!Check(result.size() == 2, "two batches loaded")) return false;
  if (!Check(result[0].marketdata_snapshots.size() == 2,
             "b1@100 should have two matching snapshots")) return false;
  if (!Check(result[1].marketdata_snapshots.empty(),
             "b2@250 has no matching snapshot")) return false;
  return true;
}

bool test_range_filter_and_empty() {
  FakeLoader loader;
  loader.batches = {MakeBatch("b1", 50), MakeBatch("b2", 500)};
  HistoricalBatchLoaderUseCases uc(&loader);

  HistoricalBatchLoaderUseCases::Config cfg;
  cfg.from_ms = 100;
  cfg.to_ms = 400;
  cfg.chunk_size = 100;
  auto result = uc.Run(cfg);
  if (!Check(result.empty(), "out-of-range batches must be filtered")) return false;

  // Empty range: from > to.
  cfg.from_ms = 1000;
  cfg.to_ms = 0;
  auto empty = uc.Run(cfg);
  if (!Check(empty.empty(), "from>to must short-circuit")) return false;
  return true;
}

bool test_null_loader_safe() {
  HistoricalBatchLoaderUseCases uc(nullptr);
  HistoricalBatchLoaderUseCases::Config cfg;
  cfg.from_ms = 0; cfg.to_ms = 1000; cfg.chunk_size = 10;
  auto result = uc.Run(cfg);
  return Check(result.empty(), "null loader returns no batches");
}

bool test_sink_invoked_per_batch() {
  FakeLoader loader;
  loader.batches = {MakeBatch("b1", 100), MakeBatch("b2", 200), MakeBatch("b3", 300)};

  HistoricalBatchLoaderUseCases uc(&loader);
  HistoricalBatchLoaderUseCases::Config cfg;
  cfg.from_ms = 0; cfg.to_ms = 1000; cfg.chunk_size = 100;

  std::vector<std::string> seen;
  auto sink = [&](const HistoricalBatch& hb) { seen.push_back(hb.batch_result.batch_id); };
  auto result = uc.Run(cfg, sink);

  if (!Check(seen.size() == 3, "sink invoked once per batch")) return false;
  if (!Check(seen == std::vector<std::string>{"b1", "b2", "b3"},
             "sink receives batches in deterministic order")) return false;
  if (!Check(result.size() == 3, "result still returned alongside sink")) return false;
  return true;
}

bool test_stats_accumulation() {
  FakeLoader loader;
  loader.batches = {MakeBatch("b1", 10), MakeBatch("b2", 20)};
  loader.fills = {MakeFill("b1", "o1", 10, 1.0), MakeFill("b2", "o2", 20, 2.0)};
  loader.snapshots = {MakeSnapshot(10, "v", "S")};

  HistoricalBatchLoaderUseCases uc(&loader);
  HistoricalBatchLoaderUseCases::Config cfg;
  cfg.from_ms = 0; cfg.to_ms = 100; cfg.chunk_size = 5;
  uc.Run(cfg);
  uc.Run(cfg);

  auto s = uc.GetStats();
  if (!Check(s.batches_loaded == 4, "batches_loaded accumulates across runs")) return false;
  if (!Check(s.fills_loaded == 4, "fills_loaded accumulates")) return false;
  if (!Check(s.snapshots_loaded == 2, "snapshots_loaded accumulates")) return false;
  if (!Check(s.last_batch_id == "b2", "last_batch_id reflects latest run")) return false;
  if (!Check(s.chunks_read >= 6, "chunks_read counts each chunk request")) return false;
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

  run("test_deterministic_order", test_deterministic_order);
  run("test_chunked_loading", test_chunked_loading);
  run("test_chunk_size_exact_multiple_terminates", test_chunk_size_exact_multiple_terminates);
  run("test_fills_join_by_batch_id", test_fills_join_by_batch_id);
  run("test_marketdata_join_by_timestamp", test_marketdata_join_by_timestamp);
  run("test_range_filter_and_empty", test_range_filter_and_empty);
  run("test_null_loader_safe", test_null_loader_safe);
  run("test_sink_invoked_per_batch", test_sink_invoked_per_batch);
  run("test_stats_accumulation", test_stats_accumulation);

  if (all_passed) {
    std::cout << "[OK] backtest_historical_batch_loader_test passed (9 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
