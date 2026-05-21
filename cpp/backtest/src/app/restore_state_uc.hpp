#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app/replay_orchestration_ports.hpp"
#include "app/replay_step_journal.hpp"
#include "app/shadow_ledger_port.hpp"

namespace cex::backtest::app {

// Snapshot of rolling agent state reconstructed by RestoreState. Mirrors the
// fields that F15-BACKTEST-5 must restore before batch k:
//   * positions / balances             — taken from shadow ledger.
//   * cum_pnl                          — running sum of per-batch pnl.
//   * agent_state                      — rolling metrics (avg pnl, fill rate,
//                                        sharpe accumulators, drawdown, etc.).
//   * risk_alerts_count                — count of soft/hard risk failures so far.
//   * last_action                      — last AgentLog entry written (mirror of
//                                        the most recent step in the journal).
struct RollingAgentMetrics {
  uint32_t processed_batches{0};
  uint32_t failed_batches{0};
  double sum_pnl{0.0};
  double sum_is{0.0};
  double sum_fill_rate{0.0};
  double sum_solve_time_ms{0.0};
  double cum_pnl{0.0};
  double peak_cum_pnl{0.0};
  double max_drawdown{0.0};
};

struct RestoredAgentState {
  std::map<std::string, std::string> balances;
  std::map<std::string, std::string> positions;
  RollingAgentMetrics rolling;
  uint32_t risk_alerts_count{0};
  std::optional<AgentLogEntry> last_action;
};

// Port for reading previously persisted AgentLog rows for a session.
// Used by RestoreState in the "cold" path: the in-process journal is empty
// (e.g. service restart, retry from batch k), but ClickHouse already has logs
// from the previous incarnation of the session. The reader must return rows
// ordered by batch_seq ascending and may skip entries with batch_seq beyond
// `up_to_batch_seq_exclusive`.
class IAgentLogReader {
 public:
  virtual ~IAgentLogReader() = default;
  virtual std::vector<AgentLogEntry> ReadLogsUpTo(
      const std::string& session_id, uint32_t up_to_batch_seq_exclusive) = 0;
};

// F15-BACKTEST-5. RestoreState use case.
//
// Re-creates the rolling agent state and shadow-ledger position required to
// resume a replay session at a target batch. Two restore paths are supported:
//
//   * Warm restore: the in-memory ReplayStepJournal already holds steps for
//     this session (typical retry inside the same backtest process). The
//     journal is truncated from the target batch and rolling metrics are
//     recomputed from the surviving journal entries; the shadow ledger is
//     rolled back via RestoreBeforeBatch.
//
//   * Cold restore: the journal is empty (process restart / retry across
//     services). Persisted AgentLog rows are read via IAgentLogReader and
//     re-injected into the journal so subsequent batches see consistent
//     batch_seq numbering and rolling metrics. The shadow ledger is rolled
//     back to the checkpoint immediately before the target batch.
//
// The use case never advances the replay; it only restores state. A
// successful restore produces a RestoredAgentState whose rolling metrics
// match what the journal would have produced if the session had run linearly
// up to (but not including) the target batch.
class RestoreState {
 public:
  enum class Mode {
    kAuto,  // Pick warm or cold based on journal contents.
    kWarm,
    kCold,
  };

  enum class Status {
    kOk,
    kJournalMissing,        // Cold restore requested but reader unavailable.
    kCheckpointMissing,     // Shadow ledger has no checkpoint for target batch.
    kShadowRestoreFailed,   // Ledger refused to roll back.
    kNamespaceMissing,      // Shadow namespace does not exist.
    kInvalidArgument,
  };

  struct Dependencies {
    IShadowLedger* shadow_ledger{nullptr};
    ReplayStepJournal* journal{nullptr};
    IAgentLogReader* agent_log_reader{nullptr};
  };

  struct Request {
    std::string session_id;
    std::string namespace_id;
    // Batch id we are about to (re)play. Restore brings state to the point
    // *before* this batch was applied. Empty means "cold start": restore to
    // the namespace baseline and an empty rolling state.
    std::string target_batch_id;
    // Optional: how many batches were processed before the target batch in
    // the original run. When the cold path reads agent logs, only the first
    // `target_batch_seq` entries are taken into account. Ignored for warm
    // path (the journal is already authoritative there).
    std::optional<uint32_t> target_batch_seq;
    Mode mode{Mode::kAuto};
  };

  struct Result {
    Status status{Status::kOk};
    RestoredAgentState state;
    bool used_cold_path{false};
    uint32_t restored_steps{0};
    std::string error_details;
  };

  explicit RestoreState(Dependencies deps);

  Result Run(const Request& request);

 private:
  Result RunWarm(const Request& request);
  Result RunCold(const Request& request);
  void ApplyShadowLedgerState(const std::string& namespace_id,
                              RestoredAgentState& state);
  static RollingAgentMetrics RollUp(const std::vector<AgentLogEntry>& logs);

  Dependencies deps_;
};

}  // namespace cex::backtest::app
