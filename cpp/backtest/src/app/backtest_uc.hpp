#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "app/batch_storage_port.hpp"
#include "app/metrics_storage_port.hpp"

namespace cex::backtest::app {

// Receives BatchResult events from Kafka, persists them for replay (F-15),
// and computes IS/PnL metrics (F04-BACKTEST-2).
class BacktestUseCases {
 public:
  struct Stats {
    uint64_t batches_received{0};
    uint64_t batches_saved{0};
    uint64_t fills_saved{0};
    uint64_t metrics_computed{0};
    std::string last_batch_id;
  };

  explicit BacktestUseCases(IBatchReplayStorage* storage = nullptr,
                            IMetricsStorage* metrics_storage = nullptr);

  void OnBatchResult(const fob::matching::v1::BatchResult& batch);
  Stats GetStats() const;

 private:
  IBatchReplayStorage* storage_{nullptr};
  IMetricsStorage* metrics_storage_{nullptr};
  mutable std::mutex mu_;
  uint64_t batches_received_{0};
  uint64_t batches_saved_{0};
  uint64_t fills_saved_{0};
  uint64_t metrics_computed_{0};
  std::string last_batch_id_;
};

}  // namespace cex::backtest::app
