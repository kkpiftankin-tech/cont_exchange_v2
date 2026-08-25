#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app/replay_reader_port.hpp"

namespace cex::backtest::app {

// Statistical summary of a distribution: mean, median, p95, p99.
struct DistributionStats {
  double mean{0};
  double median{0};
  double p95{0};
  double p99{0};
  uint32_t count{0};
};

// Per-venue/symbol quality report over a time window.
struct VenueQualityReport {
  std::string venue_id;
  std::string symbol;
  int64_t from_ms{0};
  int64_t to_ms{0};

  // Epsilon distributions across all curves in the window.
  DistributionStats epsilon1;
  DistributionStats epsilon2;
  DistributionStats epsilon3;
  DistributionStats confidence;

  // Per-level curve counts.
  uint32_t l1_count{0};
  uint32_t l2_count{0};
  uint32_t l3_count{0};
  uint32_t total_curves{0};

  // Stale rate: fraction of expected intervals without a fresh curve.
  // expected_intervals = (to_ms - from_ms) / expected_interval_ms.
  // stale_intervals = expected_intervals - actual curves.
  double stale_rate{0};

  // Venue uptime: fraction of snapshots with status == "connected".
  double uptime{0};
  uint32_t total_snapshots{0};
  uint32_t connected_snapshots{0};
};

// Pure domain calculator for historical LOB→FOB quality metrics.
struct QualityMetricsCalculator {
  // Compute distribution stats from a vector of values.
  static DistributionStats ComputeStats(std::vector<double> values);

  // Compute percentile from a sorted vector.
  static double Percentile(const std::vector<double>& sorted, double p);

  // Compute stale rate given a time window, expected interval, and actual count.
  static double ComputeStaleRate(int64_t from_ms, int64_t to_ms,
                                 double expected_interval_ms,
                                 uint32_t actual_count);

  // Build a full quality report from stored curves and snapshots.
  static VenueQualityReport BuildReport(
      const std::string& venue_id,
      const std::string& symbol,
      int64_t from_ms,
      int64_t to_ms,
      const std::vector<CurveRow>& curves,
      const std::vector<SnapshotRow>& snapshots,
      double expected_interval_ms);
};

}  // namespace cex::backtest::app
