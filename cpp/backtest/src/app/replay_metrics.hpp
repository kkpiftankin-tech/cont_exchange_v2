#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cex::backtest::app {

// Per-snapshot replay result: original curve vs replayed curve at each level.
struct ReplayCurveComparison {
  std::string venue_id;
  std::string symbol;
  int64_t event_time_ms{0};
  std::string level;  // "L1", "L2", "L3"

  // Quality: mean absolute difference between replayed and original grids.
  double bid_p_of_q_mae{0};
  double ask_p_of_q_mae{0};
  double bid_s_of_q_mae{0};
  double ask_s_of_q_mae{0};

  // Original epsilon values from stored curve.
  double original_epsilon1{0};
  double original_epsilon2{0};
  double original_epsilon3{0};
  double original_confidence{0};

  // Replayed epsilon values (recomputed from fresh pipeline run).
  double replayed_epsilon1{0};
  double replayed_epsilon2{0};
  double replayed_epsilon3{0};

  bool matched{false};  // true if original curve was found for this snapshot
};

// Aggregate replay quality metrics over a replay run.
struct ReplayRunMetrics {
  std::string venue_id;
  std::string symbol;
  int64_t from_ms{0};
  int64_t to_ms{0};

  uint32_t snapshots_processed{0};
  uint32_t curves_compared{0};
  uint32_t curves_matched{0};

  // Per-level aggregate MAE (mean of per-snapshot MAE).
  double l1_bid_p_mae{0};
  double l1_ask_p_mae{0};
  double l2_bid_p_mae{0};
  double l2_ask_p_mae{0};
  double l3_bid_p_mae{0};
  double l3_ask_p_mae{0};

  // Per-level counts.
  uint32_t l1_count{0};
  uint32_t l2_count{0};
  uint32_t l3_count{0};

  // Mean epsilon values across the run.
  double mean_epsilon1{0};
  double mean_epsilon2{0};
  double mean_epsilon3{0};
  double mean_confidence{0};
};

// Pure domain calculator for replay quality metrics.
struct ReplayMetricsCalculator {
  // Compute MAE between two double arrays of potentially different lengths.
  // Compares element-wise up to min(a.size(), b.size()).
  static double ComputeMAE(const std::vector<double>& a,
                           const std::vector<double>& b);

  // Aggregate per-comparison metrics into a run summary.
  static ReplayRunMetrics Aggregate(
      const std::string& venue_id,
      const std::string& symbol,
      int64_t from_ms,
      int64_t to_ms,
      const std::vector<ReplayCurveComparison>& comparisons);
};

}  // namespace cex::backtest::app
