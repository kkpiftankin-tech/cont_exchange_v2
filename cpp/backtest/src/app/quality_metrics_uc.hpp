#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "app/quality_metrics.hpp"
#include "app/quality_metrics_port.hpp"
#include "app/replay_reader_port.hpp"

namespace cex::backtest::app {

// Configuration for a quality metrics computation run.
struct QualityConfig {
  std::string venue_id;
  std::string symbol;
  int64_t from_ms{0};
  int64_t to_ms{0};
  // Expected interval between curves in ms (e.g. 1000 for 1 curve/sec).
  double expected_interval_ms{1000.0};
};

// Computes historical LOB→FOB quality metrics from stored data.
// Reads curves and snapshots via IReplayReader, computes quality report,
// optionally persists via IQualityMetricsStorage.
class QualityMetricsUseCases {
 public:
  struct Stats {
    uint64_t reports_computed{0};
    uint64_t reports_saved{0};
    std::string last_venue_id;
    std::string last_symbol;
  };

  explicit QualityMetricsUseCases(IReplayReader* reader = nullptr,
                                  IQualityMetricsStorage* storage = nullptr);

  // Compute quality report without persisting.
  VenueQualityReport ComputeReport(const QualityConfig& config);

  // Compute and persist quality report.
  VenueQualityReport ComputeAndSaveReport(const QualityConfig& config);

  Stats GetStats() const;

 private:
  IReplayReader* reader_{nullptr};
  IQualityMetricsStorage* storage_{nullptr};
  mutable std::mutex mu_;
  uint64_t reports_computed_{0};
  uint64_t reports_saved_{0};
  std::string last_venue_id_;
  std::string last_symbol_;
};

}  // namespace cex::backtest::app
