#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/historical_batch_loader_uc.hpp"

namespace cex::backtest::app {

enum class FailureComponent {
  kUnknown,
  kSolver,
  kMarketData,
  kRisk,
  kHistory,
  kLedger,
  kConfig,
  kShadow,
  kPipeline,
};

inline const char* FailureComponentName(FailureComponent component) {
  switch (component) {
    case FailureComponent::kSolver: return "solver";
    case FailureComponent::kMarketData: return "marketdata";
    case FailureComponent::kRisk: return "risk";
    case FailureComponent::kHistory: return "history";
    case FailureComponent::kLedger: return "ledger";
    case FailureComponent::kConfig: return "config";
    case FailureComponent::kShadow: return "shadow";
    case FailureComponent::kPipeline: return "pipeline";
    default: return "unknown";
  }
}

// Outcome of running one historical batch through the replay pipeline
// (matching backend + risk manager + shadow ledger fan-out).
//
// Concrete implementation lives in F15-CORE-1..6; this port lets the
// RunReplaySession orchestrator stay agnostic to the exact wiring.
enum class BatchOutcome {
  kOk,            // Batch processed cleanly.
  kSoftFailure,   // Local error (solver divergence, missing marketdata,
                  // soft risk alert); session continues.
  kHardFailure,   // Unrecoverable error (ledger/config/state corruption);
                  // session must transition to failed.
};

struct BatchExecutionResult {
  BatchOutcome outcome{BatchOutcome::kOk};
  double pnl{0.0};
  double is_value{0.0};
  double fill_rate{0.0};
  uint32_t fills_applied{0};
  uint32_t solve_time_ms{0};
  std::string error_details;  // Populated for soft/hard failure.
  std::string error_code;
  FailureComponent failure_component{FailureComponent::kUnknown};
  // F15-BACKTEST-6: residual norm reported by the matching solver. Kept at
  // end of the struct so existing positional initializer lists in tests
  // continue to compile.
  double residual_norm{0.0};
  // F15-BACKTEST-7: per-batch VWAP (sum_notional / sum_qty across the
  // batch's fills). Session-level avg_vwap is computed from the canonical
  // executed_notional / executed_qty aggregates below.
  double vwap{0.0};
  // F15-METRIC-2 canonical aggregate inputs. When populated by the concrete
  // executor, ReplaySummary uses these totals instead of averaging per-batch
  // convenience metrics.
  double executed_qty{0.0};
  double requested_qty{0.0};
  double executed_notional{0.0};
  double is_weighted_sum{0.0};
  double is_weight{0.0};
  // F15 AgentLog payloads produced by the concrete pipeline executor. The
  // orchestrator persists these verbatim; empty strings are allowed for legacy
  // executors and are normalised by the ClickHouse writer.
  std::string state_json;
  std::string action_json;
  std::string fills_json;
  std::string batch_result_json;
  std::string metrics_json;
  std::string risk_status;
};

class IBatchExecutor {
 public:
  virtual ~IBatchExecutor() = default;
  // Run one historical batch through the replay pipeline against the given
  // shadow namespace. Snapshot JSON is the materialized session config so the
  // executor can pick up overrides without an extra round-trip.
  virtual BatchExecutionResult ExecuteBatch(const std::string& namespace_id,
                                            const std::string& session_config_snapshot_json,
                                            const std::string& strategy_json,
                                            const std::string& tracked_user_id,
                                            const std::string& reporting_currency,
                                            const HistoricalBatch& batch) = 0;
};

// One row written to ClickHouse `replay_agentlogs` (F15-BACKTEST-6).
//
// state_json / action_json / reward / residual_norm / fills_json /
// batch_result_json / metrics_json are populated by the orchestrator and
// persisted alongside the per-batch metrics so audit/replay UIs can replay
// the agent decision exactly.
struct AgentLogEntry {
  std::string log_id;
  std::string session_id;
  uint32_t batch_seq{0};
  std::string original_batch_id;
  int64_t event_time_ms{0};
  double pnl{0.0};
  double is_value{0.0};
  double fill_rate{0.0};
  uint32_t fills_applied{0};
  uint32_t solve_time_ms{0};
  double residual_norm{0.0};
  double reward{0.0};
  bool solver_error_flag{false};
  std::string risk_status;  // "ok" / "soft" / "hard".
  std::string error_code;
  std::string error_details;
  FailureComponent failure_component{FailureComponent::kUnknown};
  // JSON payloads (free-form, written verbatim into the ClickHouse String
  // columns). Empty string is normalised to "{}" / "[]" by the writer.
  std::string state_json;
  std::string action_json;
  std::string fills_json;
  std::string batch_result_json;
  std::string metrics_json;
  uint32_t session_config_snapshot_version{1};
};

class IAgentLogWriter {
 public:
  virtual ~IAgentLogWriter() = default;
  // Idempotent upsert keyed by (session_id, original_batch_id). A replay step
  // may be re-run after restore; the latest payload must replace the previous
  // row instead of appending a duplicate.
  virtual void WriteAgentLog(const AgentLogEntry& entry) = 0;
};

// Aggregated summary persisted on `replay_summaries` (F15-BACKTEST-7).
struct ReplaySummary {
  std::string summary_id;
  std::string session_id;
  uint32_t total_batches{0};
  uint32_t processed_batches{0};
  uint32_t failed_batches{0};
  bool partial{false};
  double total_pnl{0.0};
  double avg_pnl{0.0};
  double avg_is{0.0};
  double std_pnl{0.0};
  double sharpe{0.0};
  double avg_fill_rate{0.0};
  double avg_solve_time_ms{0.0};
  double max_drawdown{0.0};
  uint32_t total_fill_events{0};
  // F15-BACKTEST-7: cross-batch VWAP = sum(execqty_i * execprice_i) /
  // sum(execqty_i), or 0 when there is no executed quantity.
  double avg_vwap{0.0};
  std::string avgis_rule{"volume_weighted"};
  std::string decision_price_source{"marketdata_mid_with_clearprice_fallback"};
  bool no_requested_volume{false};
  std::chrono::system_clock::time_point created_at{};
};

class IReplaySummaryStore {
 public:
  virtual ~IReplaySummaryStore() = default;
  // Replace the current summary for this session_id.
  virtual void SaveSummary(const ReplaySummary& summary) = 0;
};

// Live progress / lifecycle events (F15-BACKTEST-8).
struct ReplayProgressEvent {
  std::string session_id;
  uint32_t batch_seq{0};
  uint32_t total_batches{0};
};

enum class ReplayLifecycleStatus {
  kRunning,
  kCompleted,
  kFailed,
  kCancelled,
};

struct ReplayLifecycleEvent {
  std::string session_id;
  ReplayLifecycleStatus status{ReplayLifecycleStatus::kRunning};
  std::optional<ReplaySummary> summary;
  std::optional<std::string> error_details;
  // F15-BACKTEST-8: machine-readable error code published alongside
  // error_details for failed / cancelled sessions.
  std::optional<std::string> error_code;
};

class IReplayEventPublisher {
 public:
  virtual ~IReplayEventPublisher() = default;
  virtual void PublishProgress(const ReplayProgressEvent& evt) = 0;
  virtual void PublishLifecycle(const ReplayLifecycleEvent& evt) = 0;
};

// Cooperative cancellation. The orchestrator polls IsCancelled() before each
// batch; the API layer flips it on DELETE /sessions/{id}.
class ICancellationToken {
 public:
  virtual ~ICancellationToken() = default;
  virtual bool IsCancelled() const = 0;
  virtual bool IsCancelled(const std::string& /*session_id*/) const {
    return IsCancelled();
  }
};

// Default no-op token (token always live).
class NeverCancelledToken final : public ICancellationToken {
 public:
  bool IsCancelled() const override { return false; }
  bool IsCancelled(const std::string&) const override { return false; }
};

// Atomic-flag based token, useful for tests and the API layer.
class AtomicCancellationToken final : public ICancellationToken {
 public:
  bool IsCancelled() const override { return cancelled_.load(); }
  bool IsCancelled(const std::string&) const override { return IsCancelled(); }
  void Cancel() { cancelled_.store(true); }
 private:
  std::atomic<bool> cancelled_{false};
};

}  // namespace cex::backtest::app
