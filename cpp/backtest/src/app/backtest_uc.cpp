#include "app/backtest_uc.hpp"

#include "app/metrics.hpp"
#include "cex/common/log.hpp"

namespace cex::backtest::app {

BacktestUseCases::BacktestUseCases(IBatchReplayStorage* storage,
                                   IMetricsStorage* metrics_storage)
    : storage_(storage), metrics_storage_(metrics_storage) {}

void BacktestUseCases::OnBatchResult(const fob::matching::v1::BatchResult& batch) {
  {
    std::lock_guard<std::mutex> lg(mu_);
    ++batches_received_;
    last_batch_id_ = batch.batch_id();
  }

  if (storage_ == nullptr) {
    cex::common::log_json("WARN", "Backtest replay storage is not configured",
                          {{"batch_id", batch.batch_id()}});
    return;
  }

  const bool batch_ok = storage_->SaveBatchResult(batch);
  const bool fills_ok = storage_->SaveFills(batch);

  {
    std::lock_guard<std::mutex> lg(mu_);
    if (batch_ok) ++batches_saved_;
    if (fills_ok) fills_saved_ += static_cast<uint64_t>(batch.fills_size());
  }

  // Compute and persist IS/PnL metrics (F04-BACKTEST-2).
  bool metrics_ok = false;
  if (metrics_storage_ != nullptr) {
    auto fill_metrics = MetricsCalculator::ComputeFillMetrics(batch);
    auto batch_metrics = MetricsCalculator::ComputeBatchMetrics(batch, fill_metrics);

    const bool fm_ok = metrics_storage_->SaveFillMetrics(
        batch.batch_id(), batch_metrics.event_time_ms, fill_metrics);
    const bool bm_ok = metrics_storage_->SaveBatchMetrics(batch_metrics);
    metrics_ok = fm_ok && bm_ok;

    if (metrics_ok) {
      std::lock_guard<std::mutex> lg(mu_);
      ++metrics_computed_;
    }
  }

  cex::common::log_json("INFO", "Backtest persisted batch.outputs",
                        {{"batch_id", batch.batch_id()},
                         {"fills", std::to_string(batch.fills_size())},
                         {"batch_saved", batch_ok ? "true" : "false"},
                         {"fills_saved", fills_ok ? "true" : "false"},
                         {"metrics_saved", metrics_ok ? "true" : "false"}});
  if (!batch_ok || !fills_ok) {
    cex::common::log_json("ERROR", "Failed to persist batch for replay",
                          {{"batch_id", batch.batch_id()},
                           {"batch_saved", batch_ok ? "true" : "false"},
                           {"fills_saved", fills_ok ? "true" : "false"}});
  }
}

BacktestUseCases::Stats BacktestUseCases::GetStats() const {
  std::lock_guard<std::mutex> lg(mu_);
  return Stats{
      .batches_received = batches_received_,
      .batches_saved = batches_saved_,
      .fills_saved = fills_saved_,
      .metrics_computed = metrics_computed_,
      .last_batch_id = last_batch_id_,
  };
}

}  // namespace cex::backtest::app
