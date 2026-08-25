#include "app/restore_state_uc.hpp"

#include <algorithm>

namespace cex::backtest::app {

RestoreState::RestoreState(Dependencies deps) : deps_(deps) {}

RollingAgentMetrics RestoreState::RollUp(
    const std::vector<AgentLogEntry>& logs) {
  RollingAgentMetrics m;
  for (const auto& log : logs) {
    const bool ok = !log.solver_error_flag && log.risk_status == "ok";
    if (ok) {
      ++m.processed_batches;
      m.sum_pnl += log.pnl;
      m.sum_is += log.is_value;
      m.sum_fill_rate += log.fill_rate;
      m.sum_solve_time_ms += static_cast<double>(log.solve_time_ms);
      m.cum_pnl += log.pnl;
      m.peak_cum_pnl = std::max(m.peak_cum_pnl, m.cum_pnl);
      m.max_drawdown = std::max(m.max_drawdown, m.peak_cum_pnl - m.cum_pnl);
    } else {
      ++m.failed_batches;
    }
  }
  return m;
}

void RestoreState::ApplyShadowLedgerState(const std::string& namespace_id,
                                          RestoredAgentState& state) {
  if (deps_.shadow_ledger == nullptr) return;
  auto ns = deps_.shadow_ledger->GetNamespace(namespace_id);
  if (!ns.has_value()) return;
  state.balances = ns->balances;
  state.positions = ns->positions;
}

RestoreState::Result RestoreState::Run(const Request& request) {
  Result result;

  if (request.session_id.empty() || request.namespace_id.empty()) {
    result.status = Status::kInvalidArgument;
    result.error_details = "session_id and namespace_id are required";
    return result;
  }
  if (deps_.shadow_ledger == nullptr) {
    result.status = Status::kInvalidArgument;
    result.error_details = "shadow_ledger dependency missing";
    return result;
  }
  if (!deps_.shadow_ledger->NamespaceExists(request.namespace_id)) {
    result.status = Status::kNamespaceMissing;
    result.error_details =
        "shadow namespace not found: " + request.namespace_id;
    return result;
  }

  // Cold start: no target batch, just sync from current namespace baseline.
  if (request.target_batch_id.empty()) {
    ApplyShadowLedgerState(request.namespace_id, result.state);
    return result;
  }

  Mode mode = request.mode;
  if (mode == Mode::kAuto) {
    const bool journal_has_target =
        deps_.journal != nullptr && deps_.journal->Contains(request.target_batch_id);
    mode = journal_has_target ? Mode::kWarm : Mode::kCold;
  }

  return mode == Mode::kWarm ? RunWarm(request) : RunCold(request);
}

RestoreState::Result RestoreState::RunWarm(const Request& request) {
  Result result;
  result.used_cold_path = false;

  if (deps_.journal == nullptr) {
    result.status = Status::kInvalidArgument;
    result.error_details = "warm restore requires journal dependency";
    return result;
  }
  if (!deps_.journal->Contains(request.target_batch_id)) {
    result.status = Status::kInvalidArgument;
    result.error_details =
        "journal does not contain target batch_id=" + request.target_batch_id;
    return result;
  }

  const auto checkpoint = deps_.shadow_ledger->GetCheckpoint(
      request.namespace_id, request.target_batch_id);
  if (!checkpoint.has_value()) {
    result.status = Status::kCheckpointMissing;
    result.error_details =
        "no shadow checkpoint for batch_id=" + request.target_batch_id;
    return result;
  }
  if (!deps_.shadow_ledger->RestoreBeforeBatch(request.namespace_id,
                                              request.target_batch_id)) {
    result.status = Status::kShadowRestoreFailed;
    result.error_details =
        "shadow ledger failed to roll back to batch_id=" +
        request.target_batch_id;
    return result;
  }

  // Truncate the journal so subsequent steps treat the target batch as fresh.
  (void)deps_.journal->TruncateFromBatch(request.target_batch_id);

  // Recompute rolling state from the surviving (pre-target) entries.
  std::vector<AgentLogEntry> surviving;
  surviving.reserve(deps_.journal->Size());
  for (const auto& entry : deps_.journal->OrderedEntries()) {
    surviving.push_back(entry.agent_log);
  }
  result.state.rolling = RollUp(surviving);
  uint32_t risk_alerts = 0;
  for (const auto& log : surviving) {
    if (log.risk_status != "ok") ++risk_alerts;
  }
  result.state.risk_alerts_count = risk_alerts;
  if (!surviving.empty()) {
    result.state.last_action = surviving.back();
  }
  result.restored_steps = static_cast<uint32_t>(surviving.size());

  ApplyShadowLedgerState(request.namespace_id, result.state);
  return result;
}

RestoreState::Result RestoreState::RunCold(const Request& request) {
  Result result;
  result.used_cold_path = true;

  if (deps_.agent_log_reader == nullptr) {
    result.status = Status::kJournalMissing;
    result.error_details = "cold restore requires IAgentLogReader";
    return result;
  }

  const auto checkpoint = deps_.shadow_ledger->GetCheckpoint(
      request.namespace_id, request.target_batch_id);
  if (!checkpoint.has_value()) {
    result.status = Status::kCheckpointMissing;
    result.error_details =
        "no shadow checkpoint for batch_id=" + request.target_batch_id;
    return result;
  }
  if (!deps_.shadow_ledger->RestoreBeforeBatch(request.namespace_id,
                                              request.target_batch_id)) {
    result.status = Status::kShadowRestoreFailed;
    result.error_details =
        "shadow ledger failed to roll back to batch_id=" +
        request.target_batch_id;
    return result;
  }

  // Pull previously persisted logs and trim them to (target_batch_seq) entries.
  // If the caller did not provide a sequence, replay everything before the
  // first occurrence of the target_batch_id in the persisted log.
  uint32_t up_to = request.target_batch_seq.value_or(UINT32_MAX);
  std::vector<AgentLogEntry> logs =
      deps_.agent_log_reader->ReadLogsUpTo(request.session_id, up_to);

  // Defensive: drop any entry at or beyond target_batch_id (a misbehaving
  // reader could include the target row itself).
  std::vector<AgentLogEntry> ordered;
  ordered.reserve(logs.size());
  for (auto& log : logs) {
    if (log.original_batch_id == request.target_batch_id) break;
    if (request.target_batch_seq.has_value() &&
        log.batch_seq >= *request.target_batch_seq) {
      continue;
    }
    ordered.push_back(std::move(log));
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const AgentLogEntry& a, const AgentLogEntry& b) {
              return a.batch_seq < b.batch_seq;
            });

  // Re-hydrate the journal so future steps keep the right batch_seq numbering.
  if (deps_.journal != nullptr) {
    for (const auto& log : ordered) {
      ReplayStepJournalEntry entry;
      entry.batch_id = log.original_batch_id;
      entry.agent_log = log;
      entry.execution.outcome =
          (log.solver_error_flag || log.risk_status == "hard")
              ? BatchOutcome::kHardFailure
          : log.risk_status == "soft" ? BatchOutcome::kSoftFailure
                                       : BatchOutcome::kOk;
      entry.execution.pnl = log.pnl;
      entry.execution.is_value = log.is_value;
      entry.execution.fill_rate = log.fill_rate;
      entry.execution.fills_applied = log.fills_applied;
      entry.execution.solve_time_ms = log.solve_time_ms;
      entry.execution.error_code = log.error_code;
      entry.execution.error_details = log.error_details;
      entry.execution.failure_component = log.failure_component;
      deps_.journal->Upsert(entry);
    }
  }

  result.state.rolling = RollUp(ordered);
  uint32_t risk_alerts = 0;
  for (const auto& log : ordered) {
    if (log.risk_status != "ok") ++risk_alerts;
  }
  result.state.risk_alerts_count = risk_alerts;
  if (!ordered.empty()) {
    result.state.last_action = ordered.back();
  }
  result.restored_steps = static_cast<uint32_t>(ordered.size());

  ApplyShadowLedgerState(request.namespace_id, result.state);
  return result;
}

}  // namespace cex::backtest::app
