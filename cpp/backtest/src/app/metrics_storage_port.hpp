#pragma once

#include <string>
#include <vector>

#include "app/metrics.hpp"

namespace cex::backtest::app {

// Port for persisting computed IS/PnL metrics into analytics storage.
class IMetricsStorage {
 public:
  virtual ~IMetricsStorage() = default;
  virtual bool SaveFillMetrics(const std::string& batch_id,
                               int64_t event_time_ms,
                               const std::vector<FillMetrics>& metrics) = 0;
  virtual bool SaveBatchMetrics(const BatchMetrics& metrics) = 0;
};

}  // namespace cex::backtest::app
