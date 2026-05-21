#include "app/replay_step_journal.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cex::backtest::app {

std::size_t ReplayStepJournal::Size() const {
  return order_.size();
}

bool ReplayStepJournal::Empty() const {
  return order_.empty();
}

bool ReplayStepJournal::Contains(const std::string& batch_id) const {
  return entries_.find(batch_id) != entries_.end();
}

uint32_t ReplayStepJournal::NextBatchSeq() const {
  return static_cast<uint32_t>(order_.size());
}

void ReplayStepJournal::Upsert(ReplayStepJournalEntry entry) {
  if (entry.batch_id.empty()) return;

  auto it = entries_.find(entry.batch_id);
  if (it == entries_.end()) {
    order_.push_back(entry.batch_id);
    entries_.emplace(entry.batch_id, std::move(entry));
    return;
  }
  it->second = std::move(entry);
}

bool ReplayStepJournal::TruncateFromBatch(const std::string& batch_id) {
  const auto it = std::find(order_.begin(), order_.end(), batch_id);
  if (it == order_.end()) return false;

  for (auto drop = it; drop != order_.end(); ++drop) {
    entries_.erase(*drop);
  }
  order_.erase(it, order_.end());
  return true;
}

std::optional<ReplayStepJournalEntry> ReplayStepJournal::Find(
    const std::string& batch_id) const {
  auto it = entries_.find(batch_id);
  if (it == entries_.end()) return std::nullopt;
  return it->second;
}

std::vector<ReplayStepJournalEntry> ReplayStepJournal::OrderedEntries() const {
  std::vector<ReplayStepJournalEntry> out;
  out.reserve(order_.size());
  for (const auto& batch_id : order_) {
    auto it = entries_.find(batch_id);
    if (it != entries_.end()) out.push_back(it->second);
  }
  return out;
}

// F15-BACKTEST-7. ReplaySummary aggregator.
// PnL/IS/fill/VWAP/drawdown are aggregated from the persisted AgentLog
// sequence. Soft/hard failures are counted in failed_batches separately;
// avg_solve_time is the only metric restricted to successfully processed
// batches per F-15.
ReplaySummary ReplayStepJournal::BuildSummary(const std::string& session_id,
                                             const uint32_t total_batches,
                                             const std::string& avgis_rule,
                                             const std::string& decision_price_source) const {
  ReplaySummary summary;
  summary.session_id = session_id;
  summary.total_batches = total_batches;
  summary.avgis_rule = avgis_rule.empty() ? "volume_weighted" : avgis_rule;
  summary.decision_price_source =
      decision_price_source.empty() ? "marketdata_mid_with_clearprice_fallback"
                                    : decision_price_source;

  double sum_pnl = 0.0;
  double sum_is = 0.0;
  double sum_solve_time = 0.0;
  uint32_t solve_time_count = 0;
  double sum_executed_qty = 0.0;
  double sum_requested_qty = 0.0;
  double sum_executed_notional = 0.0;
  double sum_is_weighted = 0.0;
  double sum_is_weight = 0.0;
  double cumulative_pnl = 0.0;
  double peak_pnl = 0.0;
  double max_drawdown = 0.0;
  std::vector<double> pnl_values;

  for (const auto& batch_id : order_) {
    auto it = entries_.find(batch_id);
    if (it == entries_.end()) continue;

    const auto& exec = it->second.execution;
    if (exec.outcome == BatchOutcome::kOk) {
      ++summary.processed_batches;
      sum_solve_time += static_cast<double>(exec.solve_time_ms);
      ++solve_time_count;
    } else {
      ++summary.failed_batches;
    }

    sum_pnl += exec.pnl;
    sum_is += exec.is_value;
    pnl_values.push_back(exec.pnl);
    summary.total_fill_events += exec.fills_applied;
    sum_executed_qty += exec.executed_qty;
    sum_requested_qty += exec.requested_qty;
    sum_executed_notional += exec.executed_notional;
    sum_is_weighted += exec.is_weighted_sum;
    sum_is_weight += exec.is_weight;

    cumulative_pnl += exec.pnl;
    peak_pnl = std::max(peak_pnl, cumulative_pnl);
    max_drawdown = std::max(max_drawdown, peak_pnl - cumulative_pnl);
  }

  if (!pnl_values.empty()) {
    const double n = static_cast<double>(pnl_values.size());
    summary.total_pnl = sum_pnl;
    summary.avg_pnl = sum_pnl / n;
    if (summary.avgis_rule == "simple_mean") {
      summary.avg_is = sum_is / n;
    } else {
      summary.avg_is = sum_is_weight > 0.0 ? sum_is_weighted / sum_is_weight : sum_is / n;
    }
    summary.avg_fill_rate =
        sum_requested_qty > 0.0 ? (sum_executed_qty / sum_requested_qty) * 100.0
                                : 0.0;
    summary.no_requested_volume = sum_requested_qty <= 0.0;
    double variance = 0.0;
    for (const double pnl : pnl_values) {
      const double diff = pnl - summary.avg_pnl;
      variance += diff * diff;
    }
    variance /= n;
    summary.std_pnl = std::sqrt(variance);
    if (summary.std_pnl > 0.0) {
      summary.sharpe = summary.avg_pnl / summary.std_pnl;
    }
  }
  if (solve_time_count > 0) {
    summary.avg_solve_time_ms =
        sum_solve_time / static_cast<double>(solve_time_count);
  }
  if (sum_executed_qty > 0.0) {
    summary.avg_vwap = sum_executed_notional / sum_executed_qty;
  }
  summary.max_drawdown = max_drawdown;
  return summary;
}

}  // namespace cex::backtest::app
