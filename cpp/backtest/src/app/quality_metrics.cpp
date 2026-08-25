#include "app/quality_metrics.hpp"

#include <algorithm>
#include <cmath>

namespace cex::backtest::app {

DistributionStats QualityMetricsCalculator::ComputeStats(
    std::vector<double> values) {
  DistributionStats stats;
  stats.count = static_cast<uint32_t>(values.size());
  if (values.empty()) return stats;

  double sum = 0;
  for (double v : values) sum += v;
  stats.mean = sum / static_cast<double>(values.size());

  std::sort(values.begin(), values.end());
  stats.median = Percentile(values, 0.50);
  stats.p95 = Percentile(values, 0.95);
  stats.p99 = Percentile(values, 0.99);

  return stats;
}

double QualityMetricsCalculator::Percentile(const std::vector<double>& sorted,
                                            double p) {
  if (sorted.empty()) return 0.0;
  if (sorted.size() == 1) return sorted[0];

  const double idx = p * static_cast<double>(sorted.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(idx));
  const auto hi = static_cast<std::size_t>(std::ceil(idx));
  if (lo == hi || hi >= sorted.size()) return sorted[lo];

  const double frac = idx - static_cast<double>(lo);
  return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

double QualityMetricsCalculator::ComputeStaleRate(int64_t from_ms,
                                                  int64_t to_ms,
                                                  double expected_interval_ms,
                                                  uint32_t actual_count) {
  if (expected_interval_ms <= 0 || to_ms <= from_ms) return 0.0;
  const double window_ms = static_cast<double>(to_ms - from_ms);
  const double expected = window_ms / expected_interval_ms;
  if (expected <= 0) return 0.0;
  const double stale = std::max(0.0, expected - static_cast<double>(actual_count));
  return stale / expected;
}

VenueQualityReport QualityMetricsCalculator::BuildReport(
    const std::string& venue_id,
    const std::string& symbol,
    int64_t from_ms,
    int64_t to_ms,
    const std::vector<CurveRow>& curves,
    const std::vector<SnapshotRow>& snapshots,
    double expected_interval_ms) {
  VenueQualityReport report;
  report.venue_id = venue_id;
  report.symbol = symbol;
  report.from_ms = from_ms;
  report.to_ms = to_ms;
  report.total_curves = static_cast<uint32_t>(curves.size());
  report.total_snapshots = static_cast<uint32_t>(snapshots.size());

  // Collect epsilon/confidence distributions.
  std::vector<double> eps1_vals, eps2_vals, eps3_vals, conf_vals;
  eps1_vals.reserve(curves.size());
  eps2_vals.reserve(curves.size());
  eps3_vals.reserve(curves.size());
  conf_vals.reserve(curves.size());

  for (const auto& c : curves) {
    eps1_vals.push_back(c.epsilon1);
    eps2_vals.push_back(c.epsilon2);
    eps3_vals.push_back(c.epsilon3);
    conf_vals.push_back(c.confidence);

    if (c.level == "L1") ++report.l1_count;
    else if (c.level == "L2") ++report.l2_count;
    else if (c.level == "L3") ++report.l3_count;
  }

  report.epsilon1 = ComputeStats(std::move(eps1_vals));
  report.epsilon2 = ComputeStats(std::move(eps2_vals));
  report.epsilon3 = ComputeStats(std::move(eps3_vals));
  report.confidence = ComputeStats(std::move(conf_vals));

  // Stale rate.
  report.stale_rate = ComputeStaleRate(
      from_ms, to_ms, expected_interval_ms, report.total_curves);

  // Venue uptime from snapshots.
  uint32_t connected = 0;
  for (const auto& s : snapshots) {
    if (s.status == "connected") ++connected;
  }
  report.connected_snapshots = connected;
  report.uptime = snapshots.empty()
      ? 0.0
      : static_cast<double>(connected) / static_cast<double>(snapshots.size());

  return report;
}

}  // namespace cex::backtest::app
