#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "app/historical_batch_loader_port.hpp"

namespace cex::backtest::app {

// One historical batch reconstructed from the loader: BatchResultRow joined
// with all fills sharing the same batch_id, plus market-data snapshots that
// fall on the batch's event_time_ms (best-effort, may be empty).
struct HistoricalBatch {
  HistoricalBatchResultRow batch_result;
  std::vector<HistoricalFillRow> fills;
  std::vector<HistoricalMarketdataSnapshotRow> marketdata_snapshots;
};

// F15-BACKTEST-1.
// Loads historical batches and marketdata over a time range from a backing
// IHistoricalBatchLoader (ClickHouse) in deterministic order
// (event_time_ms ASC, batch_id ASC) with chunked, memory-bounded reads.
class HistoricalBatchLoaderUseCases {
 public:
  struct Config {
    int64_t from_ms{0};
    int64_t to_ms{0};
    int64_t chunk_size{1000};
  };

  struct Stats {
    uint64_t batches_loaded{0};
    uint64_t fills_loaded{0};
    uint64_t snapshots_loaded{0};
    uint64_t chunks_read{0};
    std::string last_batch_id;
  };

  using BatchSink = std::function<void(const HistoricalBatch&)>;

  explicit HistoricalBatchLoaderUseCases(IHistoricalBatchLoader* loader = nullptr);

  // Load all batches within [from_ms, to_ms], deterministically ordered.
  // Streams reconstructed HistoricalBatch entries into `sink` if provided,
  // and accumulates them into the returned vector. Marketdata snapshots are
  // pre-loaded once per range and assigned to a batch when their
  // event_time_ms equals the batch's event_time_ms.
  std::vector<HistoricalBatch> Run(const Config& config, const BatchSink& sink = {});

  Stats GetStats() const;

 private:
  // Load all rows of one stream (batchresults / fills / marketdata) using
  // chunked reads. Stops when a chunk smaller than chunk_size is returned.
  template <typename Row, typename Loader>
  std::vector<Row> LoadAllChunked(Loader load_chunk, const Config& config);

  IHistoricalBatchLoader* loader_{nullptr};
  mutable std::mutex mu_;
  Stats stats_{};
};

}  // namespace cex::backtest::app
