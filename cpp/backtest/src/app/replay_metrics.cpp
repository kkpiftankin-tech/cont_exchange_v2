#include "app/replay_metrics.hpp"

#include <algorithm>
#include <cmath>

namespace cex::backtest::app {

double ReplayMetricsCalculator::ComputeMAE(const std::vector<double>& a,
                                           const std::vector<double>& b) {
  const auto n = std::min(a.size(), b.size());
  if (n == 0) return 0.0;
  double sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    sum += std::fabs(a[i] - b[i]);
  }
  return sum / static_cast<double>(n);
}

ReplayRunMetrics ReplayMetricsCalculator::Aggregate(
    const std::string& venue_id,
    const std::string& symbol,
    int64_t from_ms,
    int64_t to_ms,
    const std::vector<ReplayCurveComparison>& comparisons) {
  ReplayRunMetrics m;
  m.venue_id = venue_id;
  m.symbol = symbol;
  m.from_ms = from_ms;
  m.to_ms = to_ms;
  m.curves_compared = static_cast<uint32_t>(comparisons.size());

  double eps1_sum = 0, eps2_sum = 0, eps3_sum = 0, conf_sum = 0;
  uint32_t eps_count = 0;

  for (const auto& c : comparisons) {
    if (c.matched) ++m.curves_matched;

    if (c.level == "L1") {
      m.l1_bid_p_mae += c.bid_p_of_q_mae;
      m.l1_ask_p_mae += c.ask_p_of_q_mae;
      ++m.l1_count;
    } else if (c.level == "L2") {
      m.l2_bid_p_mae += c.bid_p_of_q_mae;
      m.l2_ask_p_mae += c.ask_p_of_q_mae;
      ++m.l2_count;
    } else if (c.level == "L3") {
      m.l3_bid_p_mae += c.bid_p_of_q_mae;
      m.l3_ask_p_mae += c.ask_p_of_q_mae;
      ++m.l3_count;
    }

    if (c.matched) {
      eps1_sum += c.original_epsilon1;
      eps2_sum += c.original_epsilon2;
      eps3_sum += c.original_epsilon3;
      conf_sum += c.original_confidence;
      ++eps_count;
    }
  }

  if (m.l1_count > 0) {
    m.l1_bid_p_mae /= m.l1_count;
    m.l1_ask_p_mae /= m.l1_count;
  }
  if (m.l2_count > 0) {
    m.l2_bid_p_mae /= m.l2_count;
    m.l2_ask_p_mae /= m.l2_count;
  }
  if (m.l3_count > 0) {
    m.l3_bid_p_mae /= m.l3_count;
    m.l3_ask_p_mae /= m.l3_count;
  }
  if (eps_count > 0) {
    m.mean_epsilon1 = eps1_sum / eps_count;
    m.mean_epsilon2 = eps2_sum / eps_count;
    m.mean_epsilon3 = eps3_sum / eps_count;
    m.mean_confidence = conf_sum / eps_count;
  }

  return m;
}

}  // namespace cex::backtest::app
