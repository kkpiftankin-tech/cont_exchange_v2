#pragma once

#include "app/quality_metrics.hpp"

namespace cex::backtest::app {

// Port for persisting LOB→FOB quality reports into analytics storage.
class IQualityMetricsStorage {
 public:
  virtual ~IQualityMetricsStorage() = default;
  virtual bool SaveQualityReport(const VenueQualityReport& report) = 0;
};

}  // namespace cex::backtest::app
