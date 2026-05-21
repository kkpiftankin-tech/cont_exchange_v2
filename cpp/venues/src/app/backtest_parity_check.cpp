#include "app/backtest_parity_check.hpp"

#include <sstream>
#include <string>

namespace cex::venues::app {

namespace {

using fob::execution::v1::ExecutionIntent;
using fob::execution::v1::ExecutionReport;

std::string format_decimal(const fob::common::v1::Decimal& d) {
  std::ostringstream oss;
  oss << d.units() << "@" << d.scale();
  return oss.str();
}

int64_t aligned_units(const fob::common::v1::Decimal& d, const int32_t target_scale) {
  int64_t units = d.units();
  int32_t scale = d.scale();
  while (scale < target_scale) {
    units *= 10;
    ++scale;
  }
  while (scale > target_scale) {
    units /= 10;
    --scale;
  }
  return units;
}

void compare_reports(const std::size_t intent_index,
                     const std::string& intent_id,
                     const ExecutionReport& baseline,
                     const ExecutionReport& candidate,
                     std::vector<BacktestParityCheck::FieldMismatch>* out) {
  auto push = [&](const std::string& field, const std::string& a, const std::string& b) {
    if (a == b) return;
    out->push_back(BacktestParityCheck::FieldMismatch{
        .intent_index = intent_index,
        .intent_id = intent_id,
        .field = field,
        .baseline = a,
        .candidate = b,
    });
  };

  push("status",
       std::to_string(static_cast<int>(baseline.status())),
       std::to_string(static_cast<int>(candidate.status())));
  push("filled_qty", format_decimal(baseline.filled_qty()),
       format_decimal(candidate.filled_qty()));
  push("remaining_qty", format_decimal(baseline.remaining_qty()),
       format_decimal(candidate.remaining_qty()));
  push("average_price", format_decimal(baseline.average_price()),
       format_decimal(candidate.average_price()));
  push("slippage_bps", std::to_string(baseline.slippage_bps()),
       std::to_string(candidate.slippage_bps()));

  // Fee total — compare amount and currency; absence on both sides is OK.
  const auto& base_fee = baseline.fee_total();
  const auto& cand_fee = candidate.fee_total();
  push("fee_total.cost.amount", format_decimal(base_fee.cost().amount()),
       format_decimal(cand_fee.cost().amount()));
  push("fee_total.cost.currency", base_fee.cost().currency(),
       cand_fee.cost().currency());

  push("error.code", baseline.error().code(), candidate.error().code());
}

}  // namespace

BacktestParityCheck::ParityReport BacktestParityCheck::Run(
    const std::vector<ExecutionIntent>& intents,
    const AdapterFactory& factory,
    const AdapterConfigurator& configure,
    const std::size_t runs) {
  ParityReport report;
  report.intents = intents.size();
  report.runs = runs;
  if (runs == 0 || intents.empty() || !factory) {
    report.deterministic = true;
    return report;
  }

  const int32_t qty_scale = intents.front().target_qty().scale();

  std::vector<std::vector<ExecutionReport>> per_run_reports;
  per_run_reports.reserve(runs);

  ExecuteOnVenue executor;

  for (std::size_t run_idx = 0; run_idx < runs; ++run_idx) {
    auto adapter = factory(run_idx);
    if (configure) configure(*adapter, run_idx);

    RunStats stats;
    stats.qty_scale = qty_scale;
    std::vector<ExecutionReport> run_reports;
    run_reports.reserve(intents.size());

    for (const auto& intent : intents) {
      const auto report_pb = executor.Run(intent, adapter.get());
      run_reports.push_back(report_pb);

      stats.intents_executed += 1;
      stats.total_target_qty_units +=
          aligned_units(intent.target_qty(), qty_scale);
      stats.total_filled_qty_units +=
          aligned_units(report_pb.filled_qty(), qty_scale);
    }

    stats.reconciliation_gap_units =
        stats.total_target_qty_units - stats.total_filled_qty_units;
    report.per_run.push_back(stats);
    per_run_reports.push_back(std::move(run_reports));
  }

  // Compare every later run against the first one.
  for (std::size_t run_idx = 1; run_idx < per_run_reports.size(); ++run_idx) {
    if (per_run_reports[run_idx].size() != per_run_reports.front().size()) {
      report.mismatches.push_back(FieldMismatch{
          .intent_index = 0,
          .intent_id = "",
          .field = "report_count",
          .baseline = std::to_string(per_run_reports.front().size()),
          .candidate = std::to_string(per_run_reports[run_idx].size()),
      });
      continue;
    }
    for (std::size_t i = 0; i < per_run_reports.front().size(); ++i) {
      compare_reports(i, intents[i].intent_id(),
                      per_run_reports.front()[i],
                      per_run_reports[run_idx][i],
                      &report.mismatches);
    }

    if (report.per_run[run_idx].reconciliation_gap_units !=
        report.per_run.front().reconciliation_gap_units) {
      report.mismatches.push_back(FieldMismatch{
          .intent_index = 0,
          .intent_id = "",
          .field = "reconciliation_gap_units",
          .baseline = std::to_string(report.per_run.front().reconciliation_gap_units),
          .candidate = std::to_string(report.per_run[run_idx].reconciliation_gap_units),
      });
    }
  }

  report.deterministic = report.mismatches.empty();
  return report;
}

}  // namespace cex::venues::app
