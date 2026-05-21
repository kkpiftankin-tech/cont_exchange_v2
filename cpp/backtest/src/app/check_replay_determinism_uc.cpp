#include "app/check_replay_determinism_uc.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "app/replay_orchestration_ports.hpp"
#include "app/replay_session.hpp"
#include "app/restore_state_uc.hpp"

namespace cex::backtest::app {
namespace {

constexpr const char* kValidationError = "validation_error";
constexpr const char* kDependencyError = "dependency_error";
constexpr const char* kNotFoundError = "not_found";
constexpr const char* kNoDataError = "no_data";

CheckReplayDeterminism::Result ErrorResult(std::string error_code,
                                          std::string error_message) {
  CheckReplayDeterminism::Result result;
  result.ok = false;
  result.equivalent = false;
  result.error_code = std::move(error_code);
  result.error_message = std::move(error_message);
  return result;
}

void AddMismatch(std::vector<CheckReplayDeterminism::Mismatch>& mismatches,
                 std::string scope,
                 std::string field,
                 std::string details) {
  mismatches.push_back(CheckReplayDeterminism::Mismatch{
      .scope = std::move(scope),
      .field = std::move(field),
      .details = std::move(details),
  });
}

bool EquivalentDouble(const double lhs,
                      const double rhs,
                      const double tolerance) {
  return std::fabs(lhs - rhs) <= tolerance;
}

std::optional<int64_t> ExtractJsonInt(const std::string& json,
                                      const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  std::size_t pos = json.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return std::nullopt;
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
    ++pos;
  }
  std::size_t end = pos;
  while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
  try {
    return std::stoll(json.substr(pos, end - pos));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> ExtractJsonDouble(const std::string& json,
                                        const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  std::size_t pos = json.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return std::nullopt;
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
    ++pos;
  }
  std::size_t end = pos;
  while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
  try {
    return std::stod(json.substr(pos, end - pos));
  } catch (...) {
    return std::nullopt;
  }
}

bool SameDateRange(const ReplaySession& lhs, const ReplaySession& rhs) {
  return lhs.date_range_from == rhs.date_range_from &&
         lhs.date_range_to == rhs.date_range_to;
}

bool SameBatchOrder(const std::vector<AgentLogEntry>& lhs,
                    const std::vector<AgentLogEntry>& rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].batch_seq != rhs[i].batch_seq ||
        lhs[i].original_batch_id != rhs[i].original_batch_id) {
      return false;
    }
  }
  return true;
}

void CompareAgentLogField(const AgentLogEntry& lhs,
                          const AgentLogEntry& rhs,
                          const std::string& field,
                          const bool equivalent,
                          std::vector<CheckReplayDeterminism::Mismatch>& mismatches) {
  if (equivalent) return;
  std::ostringstream details;
  details << "batch_seq=" << lhs.batch_seq
          << ", original_batch_id=" << lhs.original_batch_id;
  AddMismatch(mismatches, "agent_log", field, details.str());
}

bool CompareAgentLogs(const std::vector<AgentLogEntry>& lhs,
                      const std::vector<AgentLogEntry>& rhs,
                      const double tolerance,
                      std::vector<CheckReplayDeterminism::Mismatch>& mismatches) {
  if (lhs.size() != rhs.size()) {
    std::ostringstream details;
    details << "size " << lhs.size() << " vs " << rhs.size();
    AddMismatch(mismatches, "agent_log", "size", details.str());
    return false;
  }

  bool equivalent = true;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    const auto& a = lhs[i];
    const auto& b = rhs[i];
    if (a.batch_seq != b.batch_seq) equivalent = false;
    CompareAgentLogField(a, b, "batch_seq", a.batch_seq == b.batch_seq, mismatches);
    if (a.original_batch_id != b.original_batch_id) equivalent = false;
    CompareAgentLogField(a, b, "original_batch_id",
                         a.original_batch_id == b.original_batch_id, mismatches);
    if (a.event_time_ms != b.event_time_ms) equivalent = false;
    CompareAgentLogField(a, b, "event_time_ms", a.event_time_ms == b.event_time_ms, mismatches);
    if (!EquivalentDouble(a.pnl, b.pnl, tolerance)) equivalent = false;
    CompareAgentLogField(a, b, "pnl", EquivalentDouble(a.pnl, b.pnl, tolerance), mismatches);
    if (!EquivalentDouble(a.is_value, b.is_value, tolerance)) equivalent = false;
    CompareAgentLogField(a, b, "is_value",
                         EquivalentDouble(a.is_value, b.is_value, tolerance), mismatches);
    if (!EquivalentDouble(a.fill_rate, b.fill_rate, tolerance)) equivalent = false;
    CompareAgentLogField(a, b, "fill_rate",
                         EquivalentDouble(a.fill_rate, b.fill_rate, tolerance), mismatches);
    if (a.fills_applied != b.fills_applied) equivalent = false;
    CompareAgentLogField(a, b, "fills_applied",
                         a.fills_applied == b.fills_applied, mismatches);
    if (!EquivalentDouble(static_cast<double>(a.solve_time_ms),
                          static_cast<double>(b.solve_time_ms), tolerance)) {
      equivalent = false;
    }
    CompareAgentLogField(a, b, "solve_time_ms",
                         EquivalentDouble(static_cast<double>(a.solve_time_ms),
                                          static_cast<double>(b.solve_time_ms), tolerance),
                         mismatches);
    if (!EquivalentDouble(a.residual_norm, b.residual_norm, tolerance)) equivalent = false;
    CompareAgentLogField(a, b, "residual_norm",
                         EquivalentDouble(a.residual_norm, b.residual_norm, tolerance),
                         mismatches);
    if (!EquivalentDouble(a.reward, b.reward, tolerance)) equivalent = false;
    CompareAgentLogField(a, b, "reward",
                         EquivalentDouble(a.reward, b.reward, tolerance), mismatches);
    if (a.solver_error_flag != b.solver_error_flag) equivalent = false;
    CompareAgentLogField(a, b, "solver_error_flag",
                         a.solver_error_flag == b.solver_error_flag, mismatches);
    if (a.risk_status != b.risk_status) equivalent = false;
    CompareAgentLogField(a, b, "risk_status", a.risk_status == b.risk_status, mismatches);
    if (a.error_code != b.error_code) equivalent = false;
    CompareAgentLogField(a, b, "error_code", a.error_code == b.error_code, mismatches);
    if (a.error_details != b.error_details) equivalent = false;
    CompareAgentLogField(a, b, "error_details",
                         a.error_details == b.error_details, mismatches);
    if (a.failure_component != b.failure_component) equivalent = false;
    CompareAgentLogField(a, b, "failure_component",
                         a.failure_component == b.failure_component, mismatches);
    if (a.state_json != b.state_json) equivalent = false;
    CompareAgentLogField(a, b, "state_json", a.state_json == b.state_json, mismatches);
    if (a.action_json != b.action_json) equivalent = false;
    CompareAgentLogField(a, b, "action_json", a.action_json == b.action_json, mismatches);
    if (a.fills_json != b.fills_json) equivalent = false;
    CompareAgentLogField(a, b, "fills_json", a.fills_json == b.fills_json, mismatches);
    if (a.batch_result_json != b.batch_result_json) equivalent = false;
    CompareAgentLogField(a, b, "batch_result_json",
                         a.batch_result_json == b.batch_result_json, mismatches);
    if (a.metrics_json != b.metrics_json) equivalent = false;
    CompareAgentLogField(a, b, "metrics_json", a.metrics_json == b.metrics_json, mismatches);
  }
  return equivalent;
}

void CompareSummaryField(const std::string& field,
                         const bool equivalent,
                         std::vector<CheckReplayDeterminism::Mismatch>& mismatches) {
  if (equivalent) return;
  AddMismatch(mismatches, "summary", field, "summary mismatch");
}

bool CompareSummaries(const ReplaySummary& lhs,
                      const ReplaySummary& rhs,
                      const double tolerance,
                      std::vector<CheckReplayDeterminism::Mismatch>& mismatches) {
  bool equivalent = true;
  if (lhs.total_batches != rhs.total_batches) equivalent = false;
  CompareSummaryField("total_batches", lhs.total_batches == rhs.total_batches, mismatches);
  if (lhs.processed_batches != rhs.processed_batches) equivalent = false;
  CompareSummaryField("processed_batches",
                      lhs.processed_batches == rhs.processed_batches, mismatches);
  if (lhs.failed_batches != rhs.failed_batches) equivalent = false;
  CompareSummaryField("failed_batches",
                      lhs.failed_batches == rhs.failed_batches, mismatches);
  if (lhs.partial != rhs.partial) equivalent = false;
  CompareSummaryField("partial", lhs.partial == rhs.partial, mismatches);
  if (!EquivalentDouble(lhs.total_pnl, rhs.total_pnl, tolerance)) equivalent = false;
  CompareSummaryField("total_pnl",
                      EquivalentDouble(lhs.total_pnl, rhs.total_pnl, tolerance), mismatches);
  if (!EquivalentDouble(lhs.avg_pnl, rhs.avg_pnl, tolerance)) equivalent = false;
  CompareSummaryField("avg_pnl",
                      EquivalentDouble(lhs.avg_pnl, rhs.avg_pnl, tolerance), mismatches);
  if (!EquivalentDouble(lhs.avg_is, rhs.avg_is, tolerance)) equivalent = false;
  CompareSummaryField("avg_is",
                      EquivalentDouble(lhs.avg_is, rhs.avg_is, tolerance), mismatches);
  if (!EquivalentDouble(lhs.std_pnl, rhs.std_pnl, tolerance)) equivalent = false;
  CompareSummaryField("std_pnl",
                      EquivalentDouble(lhs.std_pnl, rhs.std_pnl, tolerance), mismatches);
  if (!EquivalentDouble(lhs.sharpe, rhs.sharpe, tolerance)) equivalent = false;
  CompareSummaryField("sharpe",
                      EquivalentDouble(lhs.sharpe, rhs.sharpe, tolerance), mismatches);
  if (!EquivalentDouble(lhs.avg_fill_rate, rhs.avg_fill_rate, tolerance)) equivalent = false;
  CompareSummaryField("avg_fill_rate",
                      EquivalentDouble(lhs.avg_fill_rate, rhs.avg_fill_rate, tolerance),
                      mismatches);
  if (!EquivalentDouble(lhs.avg_solve_time_ms, rhs.avg_solve_time_ms, tolerance)) {
    equivalent = false;
  }
  CompareSummaryField("avg_solve_time_ms",
                      EquivalentDouble(lhs.avg_solve_time_ms, rhs.avg_solve_time_ms, tolerance),
                      mismatches);
  if (!EquivalentDouble(lhs.max_drawdown, rhs.max_drawdown, tolerance)) equivalent = false;
  CompareSummaryField("max_drawdown",
                      EquivalentDouble(lhs.max_drawdown, rhs.max_drawdown, tolerance),
                      mismatches);
  if (!EquivalentDouble(lhs.avg_vwap, rhs.avg_vwap, tolerance)) equivalent = false;
  CompareSummaryField("avg_vwap",
                      EquivalentDouble(lhs.avg_vwap, rhs.avg_vwap, tolerance), mismatches);
  return equivalent;
}

}  // namespace

CheckReplayDeterminism::CheckReplayDeterminism(Dependencies deps)
    : deps_(std::move(deps)) {}

CheckReplayDeterminism::Result CheckReplayDeterminism::Run(
    const Request& request) const {
  if (deps_.session_repo == nullptr || deps_.summary_reader == nullptr ||
      deps_.agent_log_reader == nullptr) {
    return ErrorResult(kDependencyError,
                       "CheckReplayDeterminism dependencies are missing");
  }
  if (request.baseline_session_id.empty() || request.rerun_session_id.empty()) {
    return ErrorResult(kValidationError,
                       "baseline_session_id and rerun_session_id must not be empty");
  }

  const auto baseline = deps_.session_repo->GetById(request.baseline_session_id);
  if (!baseline.has_value()) {
    return ErrorResult(kNotFoundError,
                       "Replay session not found: baseline_session_id=" +
                           request.baseline_session_id);
  }
  const auto rerun = deps_.session_repo->GetById(request.rerun_session_id);
  if (!rerun.has_value()) {
    return ErrorResult(kNotFoundError,
                       "Replay session not found: rerun_session_id=" +
                           request.rerun_session_id);
  }
  if (!baseline->session_config_snapshot_json.has_value() ||
      !rerun->session_config_snapshot_json.has_value()) {
    return ErrorResult(kNoDataError,
                       "session_config_snapshot_json is missing for determinism check");
  }

  const auto baseline_snapshot_version =
      ExtractJsonInt(*baseline->session_config_snapshot_json, "snapshot_version");
  const auto rerun_snapshot_version =
      ExtractJsonInt(*rerun->session_config_snapshot_json, "snapshot_version");
  const auto baseline_random_seed =
      ExtractJsonInt(*baseline->session_config_snapshot_json, "random_seed");
  const auto rerun_random_seed =
      ExtractJsonInt(*rerun->session_config_snapshot_json, "random_seed");
  const auto baseline_tolerance =
      ExtractJsonDouble(*baseline->session_config_snapshot_json, "tolerance");
  const auto rerun_tolerance =
      ExtractJsonDouble(*rerun->session_config_snapshot_json, "tolerance");
  if (!baseline_snapshot_version.has_value() || !rerun_snapshot_version.has_value() ||
      !baseline_random_seed.has_value() || !rerun_random_seed.has_value() ||
      !baseline_tolerance.has_value() || !rerun_tolerance.has_value()) {
    return ErrorResult(kNoDataError,
                       "session_config_snapshot_json is malformed for determinism check");
  }

  const auto baseline_summary =
      deps_.summary_reader->GetSummaryBySessionId(request.baseline_session_id);
  if (!baseline_summary.has_value()) {
    return ErrorResult(kNoDataError,
                       "Replay summary not found: baseline_session_id=" +
                           request.baseline_session_id);
  }
  const auto rerun_summary =
      deps_.summary_reader->GetSummaryBySessionId(request.rerun_session_id);
  if (!rerun_summary.has_value()) {
    return ErrorResult(kNoDataError,
                       "Replay summary not found: rerun_session_id=" +
                           request.rerun_session_id);
  }

  const auto baseline_logs = deps_.agent_log_reader->ReadLogsUpTo(
      request.baseline_session_id, std::numeric_limits<uint32_t>::max());
  if (baseline_logs.empty()) {
    return ErrorResult(kNoDataError,
                       "Replay agent logs not found: baseline_session_id=" +
                           request.baseline_session_id);
  }
  const auto rerun_logs = deps_.agent_log_reader->ReadLogsUpTo(
      request.rerun_session_id, std::numeric_limits<uint32_t>::max());
  if (rerun_logs.empty()) {
    return ErrorResult(kNoDataError,
                       "Replay agent logs not found: rerun_session_id=" +
                           request.rerun_session_id);
  }

  Result result;
  result.ok = true;
  result.baseline_summary = *baseline_summary;
  result.rerun_summary = *rerun_summary;
  result.same_date_range = SameDateRange(*baseline, *rerun);
  result.same_strategy = baseline->strategy_json == rerun->strategy_json;
  result.same_snapshot_json =
      *baseline->session_config_snapshot_json == *rerun->session_config_snapshot_json;
  result.same_inputs =
      result.same_date_range && result.same_strategy && result.same_snapshot_json;
  result.same_snapshot_version =
      *baseline_snapshot_version == *rerun_snapshot_version;
  result.same_random_seed = *baseline_random_seed == *rerun_random_seed;
  result.same_tolerance =
      EquivalentDouble(*baseline_tolerance, *rerun_tolerance, 0.0);
  result.tolerance_used = std::max(*baseline_tolerance, *rerun_tolerance);
  result.same_batch_order = SameBatchOrder(baseline_logs, rerun_logs);
  if (!result.same_date_range) {
    AddMismatch(result.mismatches, "inputs", "date_range", "date range mismatch");
  }
  if (!result.same_strategy) {
    AddMismatch(result.mismatches, "inputs", "strategy_json", "strategy mismatch");
  }
  if (!result.same_snapshot_json) {
    AddMismatch(result.mismatches, "inputs", "session_config_snapshot_json",
                "snapshot json mismatch");
  }
  if (!result.same_snapshot_version) {
    AddMismatch(result.mismatches, "config", "snapshot_version",
                "snapshot_version mismatch");
  }
  if (!result.same_random_seed) {
    AddMismatch(result.mismatches, "config", "random_seed", "random_seed mismatch");
  }
  if (!result.same_tolerance) {
    AddMismatch(result.mismatches, "config", "tolerance", "tolerance mismatch");
  }
  if (!result.same_batch_order) {
    AddMismatch(result.mismatches, "agent_log", "batch_order",
                "batch order mismatch");
  }

  result.agent_logs_equivalent =
      CompareAgentLogs(baseline_logs, rerun_logs, result.tolerance_used,
                       result.mismatches);
  result.summary_equivalent =
      CompareSummaries(*baseline_summary, *rerun_summary, result.tolerance_used,
                       result.mismatches);
  result.equivalent =
      result.same_inputs &&
      result.same_snapshot_version &&
      result.same_random_seed &&
      result.same_tolerance &&
      result.same_batch_order &&
      result.agent_logs_equivalent &&
      result.summary_equivalent;

  if (!result.equivalent) {
    result.error_code = "not_equivalent";
    result.error_message = "Replay determinism check failed";
  }
  return result;
}

}  // namespace cex::backtest::app
