#include "app/historical_batch_loader_uc.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace cex::backtest::app {

HistoricalBatchLoaderUseCases::HistoricalBatchLoaderUseCases(
    IHistoricalBatchLoader* loader)
    : loader_(loader) {}

template <typename Row, typename Loader>
std::vector<Row> HistoricalBatchLoaderUseCases::LoadAllChunked(
    Loader load_chunk, const Config& config) {
  std::vector<Row> all;
  if (config.chunk_size <= 0) return all;
  int64_t offset = 0;
  while (true) {
    auto chunk = load_chunk(config.from_ms, config.to_ms, offset, config.chunk_size);
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.chunks_read;
    }
    const int64_t got = static_cast<int64_t>(chunk.size());
    all.insert(all.end(),
               std::make_move_iterator(chunk.begin()),
               std::make_move_iterator(chunk.end()));
    if (got < config.chunk_size) break;
    offset += got;
  }
  return all;
}

std::vector<HistoricalBatch> HistoricalBatchLoaderUseCases::Run(
    const Config& config, const BatchSink& sink) {
  std::vector<HistoricalBatch> result;
  if (loader_ == nullptr) return result;
  if (config.from_ms > config.to_ms) return result;

  auto batches = LoadAllChunked<HistoricalBatchResultRow>(
      [this](int64_t f, int64_t t, int64_t o, int64_t l) {
        return loader_->LoadBatchResults(f, t, o, l);
      },
      config);
  auto fills = LoadAllChunked<HistoricalFillRow>(
      [this](int64_t f, int64_t t, int64_t o, int64_t l) {
        return loader_->LoadFills(f, t, o, l);
      },
      config);
  auto snapshots = LoadAllChunked<HistoricalMarketdataSnapshotRow>(
      [this](int64_t f, int64_t t, int64_t o, int64_t l) {
        return loader_->LoadMarketdataSnapshots(f, t, o, l);
      },
      config);

  // Enforce determinism even if backing storage drifts.
  std::sort(batches.begin(), batches.end(),
            [](const HistoricalBatchResultRow& a, const HistoricalBatchResultRow& b) {
              if (a.event_time_ms != b.event_time_ms) return a.event_time_ms < b.event_time_ms;
              return a.batch_id < b.batch_id;
            });
  std::sort(fills.begin(), fills.end(),
            [](const HistoricalFillRow& a, const HistoricalFillRow& b) {
              if (a.event_time_ms != b.event_time_ms) return a.event_time_ms < b.event_time_ms;
              if (a.batch_id != b.batch_id) return a.batch_id < b.batch_id;
              return a.order_id < b.order_id;
            });
  std::sort(snapshots.begin(), snapshots.end(),
            [](const HistoricalMarketdataSnapshotRow& a,
               const HistoricalMarketdataSnapshotRow& b) {
              if (a.event_time_ms != b.event_time_ms) return a.event_time_ms < b.event_time_ms;
              if (a.venue_id != b.venue_id) return a.venue_id < b.venue_id;
              return a.symbol < b.symbol;
            });

  std::unordered_map<std::string, std::vector<HistoricalFillRow>> fills_by_batch;
  fills_by_batch.reserve(batches.size());
  for (auto& fill : fills) {
    fills_by_batch[fill.batch_id].push_back(std::move(fill));
  }

  std::unordered_map<int64_t, std::vector<HistoricalMarketdataSnapshotRow>> snapshots_by_ts;
  snapshots_by_ts.reserve(snapshots.size());
  for (auto& snap : snapshots) {
    snapshots_by_ts[snap.event_time_ms].push_back(std::move(snap));
  }

  result.reserve(batches.size());
  uint64_t fills_total = 0;
  uint64_t snapshots_total = 0;
  for (auto& br : batches) {
    HistoricalBatch hb;
    const std::string batch_id = br.batch_id;
    const int64_t ts = br.event_time_ms;
    hb.batch_result = std::move(br);
    auto fit = fills_by_batch.find(batch_id);
    if (fit != fills_by_batch.end()) {
      // Duplicate batch_result rows represent reruns/replacements and each
      // attempt still needs the same historical fills for input validation.
      hb.fills = fit->second;
      fills_total += hb.fills.size();
    }
    auto sit = snapshots_by_ts.find(ts);
    if (sit != snapshots_by_ts.end()) {
      hb.marketdata_snapshots = sit->second;
      snapshots_total += hb.marketdata_snapshots.size();
    }
    if (sink) sink(hb);
    result.push_back(std::move(hb));
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.batches_loaded += result.size();
    stats_.fills_loaded += fills_total;
    stats_.snapshots_loaded += snapshots_total;
    if (!result.empty()) stats_.last_batch_id = result.back().batch_result.batch_id;
  }

  return result;
}

HistoricalBatchLoaderUseCases::Stats HistoricalBatchLoaderUseCases::GetStats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

}  // namespace cex::backtest::app
