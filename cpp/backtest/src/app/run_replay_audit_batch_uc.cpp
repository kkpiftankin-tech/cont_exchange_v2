#include "app/run_replay_audit_batch_uc.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace cex::backtest::app {
namespace {

RunReplayAuditBatch::Result ErrorResult(std::string error_code,
                                        std::string error_details) {
  RunReplayAuditBatch::Result result;
  result.ok = false;
  result.error_code = std::move(error_code);
  result.error_details = std::move(error_details);
  return result;
}

std::string NormalizeJsonString(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  bool in_string = false;
  bool escaped = false;

  for (char ch : input) {
    if (escaped) {
      out.push_back(ch);
      escaped = false;
      continue;
    }
    if (in_string && ch == '\\') {
      out.push_back(ch);
      escaped = true;
      continue;
    }
    if (ch == '"') {
      out.push_back(ch);
      in_string = !in_string;
      continue;
    }
    if (!in_string && std::isspace(static_cast<unsigned char>(ch)) != 0) {
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

std::string FormatDouble(const double value) {
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
}

std::string CanonicalizeFill(const HistoricalFillRow& fill) {
  std::ostringstream out;
  out << fill.order_id << '|'
      << fill.user_id << '|'
      << fill.symbol << '|'
      << fill.base << '|'
      << fill.quote << '|'
      << fill.side << '|'
      << FormatDouble(fill.executed_qty) << '|'
      << FormatDouble(fill.price) << '|'
      << FormatDouble(fill.executed_notional) << '|'
      << FormatDouble(fill.fee_amount) << '|'
      << fill.fee_currency << '|'
      << fill.liquidity_source << '|'
      << fill.venue_id << '|'
      << fill.snapshot_id << '|'
      << fill.curve_id;
  return out.str();
}

std::string CanonicalizeFills(std::vector<HistoricalFillRow> fills) {
  std::sort(fills.begin(), fills.end(), [](const auto& lhs, const auto& rhs) {
    return CanonicalizeFill(lhs) < CanonicalizeFill(rhs);
  });

  std::ostringstream out;
  for (size_t i = 0; i < fills.size(); ++i) {
    if (i > 0) out << '\n';
    out << CanonicalizeFill(fills[i]);
  }
  return out.str();
}

std::string FoldRiskStatus(const std::vector<HistoricalRiskEventRow>& events) {
  bool has_events = false;
  bool hard = false;

  for (const auto& evt : events) {
    if (evt.batch_id.empty()) continue;
    has_events = true;
    if (evt.event_type == "margin_call" || evt.event_type == "liquidation" ||
        evt.event_type == "kill_switch") {
      hard = true;
    }
  }

  if (hard) return "hard";
  if (has_events) return "alert";
  return "ok";
}

AuditBatchSnapshot BuildProductionSnapshot(
    const HistoricalBatchResultRow& batch_row,
    std::vector<HistoricalFillRow> fills,
    const std::vector<HistoricalRiskEventRow>& risk_events) {
  AuditBatchSnapshot snapshot;
  snapshot.batch_id = batch_row.batch_id;
  snapshot.clear_prices_json = NormalizeJsonString(batch_row.clear_prices_json);
  snapshot.executed_rates_json = NormalizeJsonString(batch_row.executed_rates_json);
  snapshot.fills = std::move(fills);
  snapshot.residual_norm = batch_row.residual_norm;
  snapshot.solve_time_ms = batch_row.solve_time_ms;
  snapshot.risk_status = FoldRiskStatus(risk_events);
  return snapshot;
}

ReplayAuditDiff BuildDiff(const AuditBatchSnapshot& production,
                          const AuditBatchSnapshot& replay,
                          const double tolerance) {
  ReplayAuditDiff diff;

  diff.clear_prices.production_value = production.clear_prices_json;
  diff.clear_prices.replay_value = NormalizeJsonString(replay.clear_prices_json);
  diff.clear_prices.equivalent =
      diff.clear_prices.production_value == diff.clear_prices.replay_value;

  diff.executed_rates.production_value = production.executed_rates_json;
  diff.executed_rates.replay_value = NormalizeJsonString(replay.executed_rates_json);
  diff.executed_rates.equivalent =
      diff.executed_rates.production_value == diff.executed_rates.replay_value;

  diff.fills.production_value = CanonicalizeFills(production.fills);
  diff.fills.replay_value = CanonicalizeFills(replay.fills);
  diff.fills.equivalent = diff.fills.production_value == diff.fills.replay_value;

  diff.residual_norm.production_value = production.residual_norm;
  diff.residual_norm.replay_value = replay.residual_norm;
  diff.residual_norm.abs_diff =
      std::abs(production.residual_norm - replay.residual_norm);
  diff.residual_norm.equivalent = diff.residual_norm.abs_diff <= tolerance;

  diff.solve_time_ms.production_value = production.solve_time_ms;
  diff.solve_time_ms.replay_value = replay.solve_time_ms;
  diff.solve_time_ms.abs_diff =
      production.solve_time_ms >= replay.solve_time_ms
          ? production.solve_time_ms - replay.solve_time_ms
          : replay.solve_time_ms - production.solve_time_ms;
  diff.solve_time_ms.equivalent =
      production.solve_time_ms == replay.solve_time_ms;

  diff.risk_status.production_value = production.risk_status;
  diff.risk_status.replay_value = replay.risk_status;
  diff.risk_status.equivalent =
      diff.risk_status.production_value == diff.risk_status.replay_value;

  // solve_time_ms is reported in diff but does not participate in the
  // equivalence verdict of the first MVP audit-mode cut.
  diff.equivalent = diff.clear_prices.equivalent &&
                    diff.executed_rates.equivalent &&
                    diff.fills.equivalent &&
                    diff.residual_norm.equivalent &&
                    diff.risk_status.equivalent;
  return diff;
}

}  // namespace

RunReplayAuditBatch::RunReplayAuditBatch(Dependencies deps, Clock clock)
    : deps_(std::move(deps)), clock_(std::move(clock)) {
  if (!clock_) {
    clock_ = []() { return std::chrono::system_clock::now(); };
  }
}

std::chrono::system_clock::time_point RunReplayAuditBatch::Now() const {
  return clock_();
}

RunReplayAuditBatch::Result RunReplayAuditBatch::Run(
    const Request& request) const {
  if (deps_.config_builder == nullptr || deps_.shadow_init == nullptr ||
      deps_.audit_loader == nullptr || deps_.audit_executor == nullptr) {
    return ErrorResult("dependency_error",
                       "RunReplayAuditBatch dependencies missing");
  }
  if (request.audit_run_id.empty()) {
    return ErrorResult("validation_error", "audit_run_id is required");
  }
  if (request.batch_id.empty()) {
    return ErrorResult("validation_error", "batch_id is required");
  }
  if (request.tracked_user_id.empty()) {
    return ErrorResult("validation_error", "tracked_user_id is required");
  }

  const auto snapshot = deps_.config_builder->Build(request.config_request);
  if (!snapshot.ok) {
    return ErrorResult("config_snapshot_failed",
                       "Config snapshot failed: " + snapshot.error);
  }

  ShadowNamespaceInitializer::Request ns_req;
  ns_req.session_id = request.audit_run_id;
  ns_req.tracked_user_id = request.tracked_user_id;
  ns_req.reporting_currency = request.reporting_currency;
  ns_req.mode = ShadowNamespaceInitializer::Mode::kEmptySandbox;
  ns_req.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             Now().time_since_epoch())
                             .count();

  const auto ns_result = deps_.shadow_init->Init(ns_req);
  if (!ns_result.ok) {
    return ErrorResult("shadow_namespace_init_failed",
                       "Shadow namespace init failed: " + ns_result.error);
  }

  const auto batch_rows = deps_.audit_loader->LoadBatchResultsById(request.batch_id);
  if (batch_rows.empty()) {
    return ErrorResult("no_data", "No production batch found for batch_id=" +
                                      request.batch_id);
  }
  if (batch_rows.size() != 1) {
    return ErrorResult(
        "history_corrupt",
        "Expected exactly one batch_result row for batch_id=" + request.batch_id);
  }

  const auto fills =
      deps_.audit_loader->LoadFillsByBatchIds(std::vector<std::string>{request.batch_id});
  const auto risk_events = deps_.audit_loader->LoadRiskEventsByBatchId(request.batch_id);
  if (batch_rows.front().fills_count > fills.size()) {
    return ErrorResult("history_incomplete",
                       "Historical fills missing for batch_id=" + request.batch_id);
  }

  HistoricalBatch historical_batch;
  historical_batch.batch_result = batch_rows.front();
  historical_batch.fills = fills;
  historical_batch.marketdata_snapshots =
      deps_.audit_loader->LoadMarketdataSnapshotsByEventTimes(
          std::vector<int64_t>{batch_rows.front().event_time_ms});

  const auto replay = deps_.audit_executor->ExecuteAuditBatch(
      ns_result.namespace_id, snapshot.snapshot.snapshot_json, historical_batch);
  if (!replay.ok) {
    return ErrorResult(
        replay.error_code.empty() ? "audit_execution_failed" : replay.error_code,
        replay.error_details.empty() ? "Audit executor failed" : replay.error_details);
  }

  Result result;
  result.ok = true;
  result.config_snapshot = snapshot.snapshot;
  result.production_snapshot =
      BuildProductionSnapshot(batch_rows.front(), fills, risk_events);
  result.replay_snapshot = replay.snapshot;
  result.diff = BuildDiff(result.production_snapshot,
                          result.replay_snapshot,
                          snapshot.snapshot.tolerance);
  result.equivalent = result.diff.equivalent;
  return result;
}

}  // namespace cex::backtest::app
