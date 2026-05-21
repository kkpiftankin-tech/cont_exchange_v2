#include "app/run_replay_session_uc.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <exception>
#include <optional>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "app/replay_step_journal.hpp"
#include "app/replay_config_snapshot.hpp"
#include "app/rbac_engine.hpp"

namespace cex::backtest::app {
namespace {

ReplayLifecycleStatus ToLifecycle(ReplaySessionStatus status) {
  switch (status) {
    case ReplaySessionStatus::kCompleted: return ReplayLifecycleStatus::kCompleted;
    case ReplaySessionStatus::kFailed: return ReplayLifecycleStatus::kFailed;
    case ReplaySessionStatus::kCancelled: return ReplayLifecycleStatus::kCancelled;
    default: return ReplayLifecycleStatus::kRunning;
  }
}

std::string ComposeError(const std::string& error_code,
                         const std::string& error_details) {
  if (error_code.empty()) return error_details;
  if (error_details.empty()) return error_code;
  return error_code + ": " + error_details;
}

BatchExecutionResult MakeFailureResult(BatchOutcome outcome,
                                       FailureComponent component,
                                       std::string error_code,
                                       std::string error_details) {
  BatchExecutionResult result;
  result.outcome = outcome;
  result.error_code = std::move(error_code);
  result.error_details = ComposeError(result.error_code, error_details);
  result.failure_component = component;
  return result;
}

enum class RewardMode {
  kIncrementalPnl,
  kNegativeIs,
};

std::string NormalizeMode(std::string raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char ch : raw) {
    if (ch == '_' || ch == '-' || ch == ' ') continue;
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

std::optional<std::string> JsonString(const nlohmann::json& node,
                                      std::initializer_list<const char*> keys) {
  if (!node.is_object()) return std::nullopt;
  for (const char* key : keys) {
    if (node.contains(key) && node.at(key).is_string() &&
        !node.at(key).get<std::string>().empty()) {
      return node.at(key).get<std::string>();
    }
  }
  return std::nullopt;
}

std::optional<RewardMode> RewardModeFromSnapshot(const std::string& snapshot_json,
                                                 std::string* error) {
  if (snapshot_json.empty()) return RewardMode::kIncrementalPnl;
  const auto parsed = nlohmann::json::parse(snapshot_json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return RewardMode::kIncrementalPnl;
  }

  std::optional<std::string> raw_mode =
      JsonString(parsed, {"rewardmode", "reward_mode", "mode"});
  if (!raw_mode.has_value() && parsed.contains("reward_config") &&
      parsed.at("reward_config").is_object()) {
    const auto& reward_config = parsed.at("reward_config");
    raw_mode = JsonString(reward_config, {"mode"});
    const auto body_json = JsonString(reward_config, {"body_json", "bodyJson"});
    if (!raw_mode.has_value() && body_json.has_value()) {
      const auto body = nlohmann::json::parse(*body_json, nullptr, false);
      raw_mode = JsonString(body, {"mode", "rewardmode", "reward_mode"});
    }
  }
  if (!raw_mode.has_value()) return RewardMode::kIncrementalPnl;

  const std::string mode = NormalizeMode(*raw_mode);
  if (mode == "incrementalpnl") {
    return RewardMode::kIncrementalPnl;
  }
  if (mode == "is" || mode == "negativeis" || mode == "minusis" ||
      mode == "shortfall") {
    return RewardMode::kNegativeIs;
  }
  if (error != nullptr) {
    *error = "Unsupported reward mode in replay snapshot: " + *raw_mode;
  }
  return std::nullopt;
}

std::string SnapshotStringOrDefault(const std::string& snapshot_json,
                                    std::initializer_list<const char*> keys,
                                    const std::string& fallback) {
  if (snapshot_json.empty()) return fallback;
  const auto parsed = nlohmann::json::parse(snapshot_json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) return fallback;
  auto value = JsonString(parsed, keys);
  return value.has_value() && !value->empty() ? *value : fallback;
}

double ComputeReward(const BatchExecutionResult& exec, RewardMode mode) {
  if (exec.outcome != BatchOutcome::kOk) return 0.0;
  switch (mode) {
    case RewardMode::kNegativeIs:
      return -exec.is_value;
    case RewardMode::kIncrementalPnl:
    default:
      return exec.pnl;
  }
}

std::optional<BatchExecutionResult> ValidateBatchInputs(
    const HistoricalBatch& batch) {
  if (batch.batch_result.batch_id.empty()) {
    return MakeFailureResult(
        BatchOutcome::kHardFailure,
        FailureComponent::kHistory,
        "history_corrupt",
        "historical batch_id is empty");
  }
  if (batch.batch_result.fills_count > 0 &&
      batch.fills.size() < batch.batch_result.fills_count) {
    return MakeFailureResult(
        BatchOutcome::kSoftFailure,
        FailureComponent::kHistory,
        "history_incomplete",
        "historical fills missing for batch_id=" + batch.batch_result.batch_id);
  }
  return std::nullopt;
}

uint32_t CountUniqueBatchIds(const std::vector<HistoricalBatch>& history) {
  std::unordered_set<std::string> batch_ids;
  batch_ids.reserve(history.size());
  for (const auto& batch : history) {
    batch_ids.insert(batch.batch_result.batch_id);
  }
  return static_cast<uint32_t>(batch_ids.size());
}

}  // namespace

RunReplaySession::RunReplaySession(Dependencies deps, Clock clock)
    : deps_(deps), clock_(std::move(clock)) {
  if (!clock_) {
    clock_ = []() { return std::chrono::system_clock::now(); };
  }
}

std::chrono::system_clock::time_point RunReplaySession::Now() const {
  return clock_();
}

RunReplaySession::Result RunReplaySession::FailFromInit(
    const std::string& session_id,
    const FailureComponent component,
    const std::string& error_code,
    const std::string& error) {
  Result result;
  result.final_status = ReplaySessionStatus::kFailed;
  result.error_details = ComposeError(error_code, error);
  result.summary.session_id = session_id;
  result.summary.partial = true;

  if (deps_.session_repo != nullptr) {
    ReplaySessionStatePatch patch;
    patch.status = ReplaySessionStatus::kFailed;
    patch.completed_at = Now();
    patch.error_details = result.error_details;
    deps_.session_repo->UpdateState(session_id, patch);
  }
  if (deps_.runtime_metrics != nullptr) {
    deps_.runtime_metrics->ObserveFailure("hard", component, result.error_details);
  }
  if (deps_.summary_store != nullptr) {
    deps_.summary_store->SaveSummary(result.summary);
  }
  if (deps_.event_publisher != nullptr) {
    ReplayLifecycleEvent evt;
    evt.session_id = session_id;
    evt.status = ReplayLifecycleStatus::kFailed;
    evt.summary = result.summary;
    evt.error_details = result.error_details;
    if (!error_code.empty()) evt.error_code = error_code;
    deps_.event_publisher->PublishLifecycle(evt);
  }
  return result;
}

RunReplaySession::Result RunReplaySession::Run(const Request& request) {
  ICancellationToken* token =
      deps_.cancellation_token != nullptr ? deps_.cancellation_token : &default_token_;

  if (deps_.session_repo == nullptr || deps_.config_builder == nullptr ||
      deps_.shadow_init == nullptr || deps_.history_loader == nullptr ||
      deps_.batch_executor == nullptr) {
    return FailFromInit(request.session_id,
                        FailureComponent::kPipeline,
                        "dependency_error",
                        "RunReplaySession dependencies missing");
  }

  // First, get the session to know the user_id
  auto session_opt = deps_.session_repo->GetById(request.session_id);
  if (!session_opt.has_value()) {
    return FailFromInit(request.session_id,
                        FailureComponent::kPipeline,
                        "session_not_found",
                        "Session not found");
  }

  auto stored_total_batches = [](const ReplaySession& session,
                                 uint32_t fallback) -> uint32_t {
    return session.total_batches.has_value() && *session.total_batches > 0
               ? static_cast<uint32_t>(*session.total_batches)
               : fallback;
  };
  auto stored_progress_batches = [](const ReplaySession& session) -> uint32_t {
    return session.progress_batches > 0
               ? static_cast<uint32_t>(session.progress_batches)
               : 0;
  };
  auto terminal_skip = [&](const ReplaySession& session,
                           uint32_t total_fallback) -> Result {
    Result skipped;
    skipped.final_status = session.status;
    skipped.summary.session_id = request.session_id;
    skipped.summary.total_batches = stored_total_batches(session, total_fallback);
    skipped.summary.processed_batches = stored_progress_batches(session);
    skipped.summary.partial = session.status != ReplaySessionStatus::kCompleted;
    skipped.error_details = session.error_details.value_or(
        "Replay session is already in terminal status");
    return skipped;
  };
  auto cancel_before_running = [&](uint32_t total_batches,
                                   uint32_t processed_batches,
                                   std::string details) -> Result {
    Result cancelled_result;
    cancelled_result.final_status = ReplaySessionStatus::kCancelled;
    cancelled_result.summary.session_id = request.session_id;
    cancelled_result.summary.total_batches = total_batches;
    cancelled_result.summary.processed_batches = processed_batches;
    cancelled_result.summary.partial = true;
    cancelled_result.error_code = "cancelled";
    cancelled_result.error_details =
        details.empty() ? "Replay session was cancelled before running"
                        : std::move(details);

    ReplaySessionStatePatch patch;
    patch.status = ReplaySessionStatus::kCancelled;
    patch.completed_at = Now();
    patch.progress_batches = static_cast<int32_t>(processed_batches);
    patch.error_details = cancelled_result.error_details;
    deps_.session_repo->UpdateState(request.session_id, patch);
    if (deps_.summary_store != nullptr) {
      deps_.summary_store->SaveSummary(cancelled_result.summary);
    }
    if (deps_.event_publisher != nullptr) {
      ReplayLifecycleEvent evt;
      evt.session_id = request.session_id;
      evt.status = ReplayLifecycleStatus::kCancelled;
      evt.summary = cancelled_result.summary;
      evt.error_details = cancelled_result.error_details;
      evt.error_code = cancelled_result.error_code;
      deps_.event_publisher->PublishLifecycle(evt);
    }
    return cancelled_result;
  };

  if (session_opt->status == ReplaySessionStatus::kCancelled ||
      session_opt->status == ReplaySessionStatus::kCompleted ||
      session_opt->status == ReplaySessionStatus::kFailed) {
    return terminal_skip(*session_opt, 0);
  }

  if (token->IsCancelled(request.session_id)) {
    return cancel_before_running(
        stored_total_batches(*session_opt, 0),
        stored_progress_batches(*session_opt),
        session_opt->error_details.value_or("Replay session was cancelled before start"));
  }
  
  if (deps_.rbac_engine) {
    auto auth = deps_.rbac_engine->CanExecuteReplaySession(
        session_opt->user_id, request.session_id);
    if (!auth.allowed) {
      return FailFromInit(request.session_id,
                          FailureComponent::kPipeline,
                          "unauthorized",
                          auth.reason.value_or("User cannot execute replay sessions"));
    }
  }

  // 1) Use the snapshot persisted by CreateReplaySession when available.
  // This keeps start/retry reproducible even if production configs change
  // after the session row is created.
  SnapshotResult snap;
  if (session_opt->session_config_snapshot_json.has_value() &&
      !session_opt->session_config_snapshot_json->empty()) {
    snap.ok = true;
    snap.snapshot.snapshot_json = *session_opt->session_config_snapshot_json;
  } else {
    snap = deps_.config_builder->Build(request.config_request);
    if (!snap.ok) {
      return FailFromInit(request.session_id,
                          FailureComponent::kConfig,
                          "config_snapshot_failed",
                          "Config snapshot failed: " + snap.error);
    }
  }
  std::string reward_mode_error;
  const auto reward_mode = RewardModeFromSnapshot(snap.snapshot.snapshot_json,
                                                  &reward_mode_error);
  if (!reward_mode.has_value()) {
    return FailFromInit(request.session_id,
                        FailureComponent::kConfig,
                        "unsupported_reward_mode",
                        reward_mode_error);
  }
  const std::string avgis_rule = SnapshotStringOrDefault(
      snap.snapshot.snapshot_json,
      {"avgis_rule", "avg_is_rule"},
      "volume_weighted");
  const std::string decision_price_source = SnapshotStringOrDefault(
      snap.snapshot.snapshot_json,
      {"decision_price_source", "decisionpricesource"},
      "marketdata_mid_with_clearprice_fallback");

  // 2) Init shadow namespace.
  ShadowNamespaceInitializer::Request ns_req;
  ns_req.session_id = request.session_id;
  ns_req.tracked_user_id = request.tracked_user_id;
  ns_req.reporting_currency = request.reporting_currency;
  ns_req.mode = ShadowNamespaceInitializer::Mode::kEmptySandbox;
  ns_req.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             Now().time_since_epoch())
                             .count();
  const auto shadow_started = std::chrono::steady_clock::now();
  auto ns_result = deps_.shadow_init->Init(ns_req);
  if (deps_.runtime_metrics != nullptr) {
    const auto shadow_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - shadow_started)
                               .count();
    deps_.runtime_metrics->ObserveShadowNamespaceInit(
        static_cast<double>(shadow_ms), ns_result.ok, ns_result.reused);
  }
  if (!ns_result.ok) {
    return FailFromInit(request.session_id,
                        FailureComponent::kShadow,
                        "shadow_namespace_init_failed",
                        "Shadow namespace init failed: " + ns_result.error);
  }

  // 3) Load history.
  const auto history_started = std::chrono::steady_clock::now();
  auto history = deps_.history_loader->Run(request.history_config);
  if (deps_.runtime_metrics != nullptr) {
    std::size_t fill_count = 0;
    std::size_t snapshot_count = 0;
    for (const auto& batch : history) {
      fill_count += batch.fills.size();
      snapshot_count += batch.marketdata_snapshots.size();
    }
    const auto history_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - history_started)
                                .count();
    deps_.runtime_metrics->ObserveHistoryLoad(
        static_cast<double>(history_ms), history.size(), fill_count, snapshot_count);
  }
  const auto total_batches = CountUniqueBatchIds(history);
  if (total_batches == 0) {
    return FailFromInit(request.session_id,
                        FailureComponent::kHistory,
                        "no_data",
                        "No historical batches loaded for requested range");
  }

  if (auto latest_session = deps_.session_repo->GetById(request.session_id);
      latest_session.has_value()) {
    if (latest_session->status == ReplaySessionStatus::kCancelled) {
      return cancel_before_running(
          stored_total_batches(*latest_session, total_batches),
          stored_progress_batches(*latest_session),
          latest_session->error_details.value_or(
              "Replay session was cancelled before running"));
    }
    if (latest_session->status == ReplaySessionStatus::kCompleted ||
        latest_session->status == ReplaySessionStatus::kFailed) {
      return terminal_skip(*latest_session, total_batches);
    }
  }

  if (token->IsCancelled(request.session_id)) {
    return cancel_before_running(total_batches,
                                 0,
                                 "Replay session was cancelled before running");
  }

  // 4) Transition session to running.
  {
    ReplaySessionStatePatch patch;
    patch.status = ReplaySessionStatus::kRunning;
    patch.started_at = Now();
    patch.total_batches = static_cast<int32_t>(total_batches);
    patch.progress_batches = 0;
    deps_.session_repo->UpdateState(request.session_id, patch);
  }
  if (deps_.event_publisher != nullptr) {
    ReplayLifecycleEvent evt;
    evt.session_id = request.session_id;
    evt.status = ReplayLifecycleStatus::kRunning;
    deps_.event_publisher->PublishLifecycle(evt);
  }

  // 5) Iterate batches.
  Result result;
  result.summary.session_id = request.session_id;
  result.summary.total_batches = total_batches;

  bool hard_failed = false;
  std::string hard_error;
  std::string hard_error_code;  // F15-BACKTEST-8: surfaced on lifecycle event.
  bool cancelled = false;
  ReplayStepJournal journal;

  for (const auto& batch : history) {
    if (token->IsCancelled(request.session_id)) {
      cancelled = true;
      break;
    }

    if (journal.Contains(batch.batch_result.batch_id)) {
      if (deps_.restore_state != nullptr) {
        RestoreState::Request rs_req;
        rs_req.session_id = request.session_id;
        rs_req.namespace_id = ns_result.namespace_id;
        rs_req.target_batch_id = batch.batch_result.batch_id;
        rs_req.mode = RestoreState::Mode::kWarm;
        const auto rs_result = deps_.restore_state->Run(rs_req);
        if (rs_result.status != RestoreState::Status::kOk) {
          hard_failed = true;
          hard_error = "RestoreState failed for batch_id=" +
                       batch.batch_result.batch_id + ": " +
                       rs_result.error_details;
          hard_error_code = "restore_state_failed";
          break;
        }
      } else if (deps_.shadow_ledger == nullptr) {
        hard_failed = true;
        hard_error =
            "Replay restart requires shadow_ledger dependency for duplicate batch_id=" +
            batch.batch_result.batch_id;
        hard_error_code = "shadow_ledger_missing";
        break;
      } else {
        const auto checkpoint = deps_.shadow_ledger->GetCheckpoint(
            ns_result.namespace_id, batch.batch_result.batch_id);
        if (checkpoint.has_value() &&
            !deps_.shadow_ledger->RestoreBeforeBatch(
                ns_result.namespace_id, batch.batch_result.batch_id)) {
          hard_failed = true;
          hard_error =
              "Failed to restore shadow namespace before batch_id=" +
              batch.batch_result.batch_id;
          hard_error_code = "shadow_namespace_restore_failed";
          break;
        }
        (void)journal.TruncateFromBatch(batch.batch_result.batch_id);
      }
    }

    const uint32_t batch_seq = journal.NextBatchSeq();

    BatchExecutionResult exec;
    if (auto invalid = ValidateBatchInputs(batch); invalid.has_value()) {
      exec = *invalid;
      if (deps_.runtime_metrics != nullptr) {
        deps_.runtime_metrics->ObserveFailure(
            exec.outcome == BatchOutcome::kSoftFailure ? "soft" : "hard",
            exec.failure_component,
            exec.error_details);
      }
    } else {
      const auto batch_started = std::chrono::steady_clock::now();
      exec = deps_.batch_executor->ExecuteBatch(
          ns_result.namespace_id,
          snap.snapshot.snapshot_json,
          session_opt->strategy_json,
          request.tracked_user_id.empty() ? session_opt->user_id : request.tracked_user_id,
          request.reporting_currency,
          batch);
      if (deps_.runtime_metrics != nullptr) {
        const auto batch_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - batch_started)
                                          .count();
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                Now().time_since_epoch())
                                .count();
        deps_.runtime_metrics->ObserveBatchExecution(
            static_cast<double>(batch_latency_ms),
            exec.solve_time_ms,
            now_ms - batch.batch_result.event_time_ms,
            exec.outcome,
            exec.failure_component,
            exec.error_details);
      }
    }

    AgentLogEntry log;
    log.log_id = request.session_id + ":" + std::to_string(batch_seq);
    log.session_id = request.session_id;
    log.batch_seq = batch_seq;
    log.original_batch_id = batch.batch_result.batch_id;
    log.event_time_ms = batch.batch_result.event_time_ms;
    log.pnl = exec.pnl;
    log.is_value = exec.is_value;
    log.fill_rate = exec.fill_rate;
    log.fills_applied = exec.fills_applied;
    log.solve_time_ms = exec.solve_time_ms;
    // F15-BACKTEST-6: persist reward / residual_norm alongside pnl. Default
    // reward policy is incrementalPnL (F15-METRIC-1 supplies others); failed
    // batches contribute zero reward.
    log.reward = ComputeReward(exec, *reward_mode);
    log.residual_norm = exec.residual_norm;
    log.solver_error_flag = exec.outcome != BatchOutcome::kOk;
    log.risk_status = !exec.risk_status.empty()
                          ? exec.risk_status
                          : exec.outcome == BatchOutcome::kOk
                                ? "ok"
                                : exec.outcome == BatchOutcome::kSoftFailure ? "soft"
                                                                             : "hard";
    log.error_code = exec.error_code;
    log.error_details = exec.error_details;
    log.failure_component = exec.failure_component;
    log.state_json = exec.state_json;
    log.action_json = exec.action_json;
    log.fills_json = exec.fills_json;
    log.batch_result_json = exec.batch_result_json;
    log.metrics_json = exec.metrics_json;
    log.session_config_snapshot_version =
        static_cast<uint32_t>(SessionConfigSnapshot::kSchemaVersion);
    if (deps_.agent_log_writer != nullptr) {
      try {
        deps_.agent_log_writer->WriteAgentLog(log);
      } catch (const std::exception& ex) {
        hard_failed = true;
        hard_error_code = "agentlog_write_failed";
        hard_error = ComposeError(
            hard_error_code,
            std::string("AgentLog write failure: ") + ex.what());
        if (deps_.runtime_metrics != nullptr) {
          deps_.runtime_metrics->ObserveFailure(
              "hard", FailureComponent::kPipeline, hard_error);
        }
        break;
      }
    }
    journal.Upsert(ReplayStepJournalEntry{
        .batch_id = batch.batch_result.batch_id,
        .agent_log = log,
        .execution = exec,
    });

    if (exec.outcome == BatchOutcome::kHardFailure) {
      hard_failed = true;
      hard_error = ComposeError(
          exec.error_code,
          exec.error_details.empty() ? "Hard failure on batch" : exec.error_details);
      hard_error_code = exec.error_code;
      break;
    }
    if (exec.outcome == BatchOutcome::kSoftFailure) {
      // Soft failure: logged, counted, but does not contribute to summary
      // metrics — session keeps going.
      const auto progress_batches = static_cast<int32_t>(journal.Size());
      if (deps_.event_publisher != nullptr) {
        ReplayProgressEvent evt;
        evt.session_id = request.session_id;
        evt.batch_seq = static_cast<uint32_t>(progress_batches);
        evt.total_batches = total_batches;
        deps_.event_publisher->PublishProgress(evt);
      }
      {
        ReplaySessionStatePatch patch;
        patch.progress_batches = progress_batches;
        deps_.session_repo->UpdateState(request.session_id, patch);
      }
      continue;
    }

    if (deps_.event_publisher != nullptr) {
      ReplayProgressEvent evt;
      evt.session_id = request.session_id;
      evt.batch_seq = static_cast<uint32_t>(journal.Size());
      evt.total_batches = total_batches;
      deps_.event_publisher->PublishProgress(evt);
    }
    {
      ReplaySessionStatePatch patch;
      patch.progress_batches = static_cast<int32_t>(journal.Size());
      deps_.session_repo->UpdateState(request.session_id, patch);
    }
  }

  // 6) Finalize summary.
  result.summary = journal.BuildSummary(request.session_id,
                                        total_batches,
                                        avgis_rule,
                                        decision_price_source);

  if (hard_failed) {
    result.final_status = ReplaySessionStatus::kFailed;
    result.error_details = hard_error;
    result.error_code = hard_error_code;
    result.summary.partial = true;
  } else if (cancelled) {
    result.final_status = ReplaySessionStatus::kCancelled;
    result.summary.partial = journal.Size() < total_batches;
  } else {
    result.final_status = ReplaySessionStatus::kCompleted;
    result.summary.partial = result.summary.failed_batches > 0 ||
                             journal.Size() < total_batches;
  }

  if (deps_.summary_store != nullptr) {
    deps_.summary_store->SaveSummary(result.summary);
  }

  // 7) Persist final state and publish lifecycle.
  {
    ReplaySessionStatePatch patch;
    patch.status = result.final_status;
    patch.completed_at = Now();
    patch.progress_batches = static_cast<int32_t>(journal.Size());
    if (!result.error_details.empty()) {
      patch.error_details = result.error_details;
    }
    deps_.session_repo->UpdateState(request.session_id, patch);
  }
  if (deps_.event_publisher != nullptr) {
    ReplayLifecycleEvent evt;
    evt.session_id = request.session_id;
    evt.status = ToLifecycle(result.final_status);
    evt.summary = result.summary;
    if (!result.error_details.empty()) evt.error_details = result.error_details;
    if (!result.error_code.empty()) evt.error_code = result.error_code;
    deps_.event_publisher->PublishLifecycle(evt);
  }
  return result;
}

}  // namespace cex::backtest::app
