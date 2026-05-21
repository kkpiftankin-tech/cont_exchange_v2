#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/check_replay_determinism_uc.hpp"
#include "app/replay_compare_ports.hpp"
#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"
#include "app/restore_state_uc.hpp"

namespace {

using cex::backtest::app::AgentLogEntry;
using cex::backtest::app::CheckReplayDeterminism;
using cex::backtest::app::FailureComponent;
using cex::backtest::app::IAgentLogReader;
using cex::backtest::app::IReplaySummaryReader;
using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionListFilter;
using cex::backtest::app::ReplaySessionRepositoryPort;
using cex::backtest::app::ReplaySessionStatePatch;
using cex::backtest::app::ReplaySessionStatus;
using cex::backtest::app::ReplaySummary;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

struct FakeSessionRepo final : public ReplaySessionRepositoryPort {
  std::unordered_map<std::string, ReplaySession> sessions;

  ReplaySession Create(const ReplaySession& session) override {
    sessions[session.session_id] = session;
    return session;
  }
  std::optional<ReplaySession> GetById(const std::string& session_id) override {
    const auto it = sessions.find(session_id);
    if (it == sessions.end()) return std::nullopt;
    return it->second;
  }
  std::vector<ReplaySession> List(const ReplaySessionListFilter&) override { return {}; }
  bool UpdateState(const std::string&, const ReplaySessionStatePatch&) override { return true; }
  std::vector<ReplaySession> GetRetryChain(const std::string&) override { return {}; }
};

struct FakeSummaryReader final : public IReplaySummaryReader {
  std::unordered_map<std::string, ReplaySummary> summaries;

  std::optional<ReplaySummary> GetSummaryBySessionId(
      const std::string& session_id) override {
    const auto it = summaries.find(session_id);
    if (it == summaries.end()) return std::nullopt;
    return it->second;
  }
};

struct FakeAgentLogReader final : public IAgentLogReader {
  std::unordered_map<std::string, std::vector<AgentLogEntry>> logs;

  std::vector<AgentLogEntry> ReadLogsUpTo(
      const std::string& session_id,
      uint32_t up_to_batch_seq_exclusive) override {
    std::vector<AgentLogEntry> out;
    const auto it = logs.find(session_id);
    if (it == logs.end()) return out;
    for (const auto& row : it->second) {
      if (row.batch_seq < up_to_batch_seq_exclusive) out.push_back(row);
    }
    return out;
  }
};

std::string MakeSnapshotJson(const int snapshot_version,
                             const int random_seed,
                             const double tolerance,
                             const std::string& tag = "default") {
  return "{\"snapshot_version\":" + std::to_string(snapshot_version) +
         ",\"solver_config\":{\"id\":\"solver\",\"tag\":\"" + tag + "\"}," +
         "\"random_seed\":" + std::to_string(random_seed) +
         ",\"tolerance\":" + std::to_string(tolerance) + "}";
}

ReplaySession MakeSession(const std::string& session_id,
                          const std::string& strategy_json,
                          const std::string& snapshot_json,
                          const int64_t from_ms = 1000,
                          const int64_t to_ms = 2000) {
  ReplaySession session;
  session.session_id = session_id;
  session.user_id = "user-1";
  session.name = session_id;
  session.strategy_json = strategy_json;
  session.date_range_from =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{from_ms}};
  session.date_range_to =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{to_ms}};
  session.solver_config_id = "solver-1";
  session.risk_limits_id = "risk-1";
  session.fee_model_json = "{}";
  session.session_config_snapshot_json = snapshot_json;
  session.status = ReplaySessionStatus::kCompleted;
  session.total_batches = 2;
  session.progress_batches = 2;
  session.created_at =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{from_ms}};
  return session;
}

ReplaySummary MakeSummary(const std::string& session_id,
                          const double total_pnl,
                          const double avg_pnl,
                          const double avg_is,
                          const double std_pnl,
                          const double sharpe,
                          const double fill_rate,
                          const double avg_solve_time_ms,
                          const double max_drawdown,
                          const double avg_vwap) {
  ReplaySummary summary;
  summary.session_id = session_id;
  summary.total_batches = 2;
  summary.processed_batches = 2;
  summary.failed_batches = 0;
  summary.partial = false;
  summary.total_pnl = total_pnl;
  summary.avg_pnl = avg_pnl;
  summary.avg_is = avg_is;
  summary.std_pnl = std_pnl;
  summary.sharpe = sharpe;
  summary.avg_fill_rate = fill_rate;
  summary.avg_solve_time_ms = avg_solve_time_ms;
  summary.max_drawdown = max_drawdown;
  summary.avg_vwap = avg_vwap;
  return summary;
}

AgentLogEntry MakeLog(const std::string& session_id,
                      const uint32_t batch_seq,
                      const std::string& batch_id,
                      const double pnl,
                      const double is_value,
                      const double fill_rate,
                      const uint32_t solve_time_ms,
                      const double residual_norm,
                      const double reward) {
  AgentLogEntry log;
  log.session_id = session_id;
  log.batch_seq = batch_seq;
  log.original_batch_id = batch_id;
  log.event_time_ms = 1000 + static_cast<int64_t>(batch_seq);
  log.pnl = pnl;
  log.is_value = is_value;
  log.fill_rate = fill_rate;
  log.fills_applied = 2;
  log.solve_time_ms = solve_time_ms;
  log.residual_norm = residual_norm;
  log.reward = reward;
  log.solver_error_flag = false;
  log.risk_status = "ok";
  log.failure_component = FailureComponent::kUnknown;
  log.state_json = "{\"cash\":\"100\"}";
  log.action_json = "{\"route\":\"internal\"}";
  log.fills_json = "[]";
  log.batch_result_json = "{\"batch_id\":\"" + batch_id + "\"}";
  log.metrics_json = "{\"latency\":1}";
  return log;
}

CheckReplayDeterminism BuildUseCase(FakeSessionRepo* repo,
                                    FakeSummaryReader* summary_reader,
                                    FakeAgentLogReader* agent_log_reader) {
  CheckReplayDeterminism::Dependencies deps;
  deps.session_repo = repo;
  deps.summary_reader = summary_reader;
  deps.agent_log_reader = agent_log_reader;
  return CheckReplayDeterminism(deps);
}

bool test_identical_rerun_within_tolerance_is_equivalent() {
  FakeSessionRepo repo;
  FakeSummaryReader summaries;
  FakeAgentLogReader logs;
  const std::string snapshot = MakeSnapshotJson(1, 7, 0.01);
  repo.sessions["baseline"] = MakeSession("baseline", R"([{"symbol":"BTCUSDT"}])", snapshot);
  repo.sessions["rerun"] = MakeSession("rerun", R"([{"symbol":"BTCUSDT"}])", snapshot);
  summaries.summaries["baseline"] =
      MakeSummary("baseline", 100.0, 50.0, -1.5, 10.0, 5.0, 0.9, 10.0, 3.0, 50010.0);
  summaries.summaries["rerun"] =
      MakeSummary("rerun", 100.005, 50.0, -1.495, 10.0, 5.0, 0.905, 10.005, 3.0, 50010.005);
  logs.logs["baseline"] = {
      MakeLog("baseline", 0, "b1", 10.0, -1.0, 0.9, 11, 0.001, 1.0),
      MakeLog("baseline", 1, "b2", 90.0, -2.0, 0.9, 9, 0.002, 2.0),
  };
  logs.logs["rerun"] = {
      MakeLog("rerun", 0, "b1", 10.005, -1.0, 0.905, 11, 0.0015, 1.0),
      MakeLog("rerun", 1, "b2", 89.995, -1.995, 0.9, 9, 0.0025, 2.005),
  };

  auto uc = BuildUseCase(&repo, &summaries, &logs);
  const auto result = uc.Run({"baseline", "rerun"});

  return Check(result.ok, "determinism check must succeed") &&
         Check(result.equivalent, "within tolerance must be equivalent") &&
         Check(result.same_inputs, "same_inputs") &&
         Check(result.same_snapshot_version, "same_snapshot_version") &&
         Check(result.same_random_seed, "same_random_seed") &&
         Check(result.same_tolerance, "same_tolerance") &&
         Check(result.same_batch_order, "same_batch_order") &&
         Check(result.agent_logs_equivalent, "agent_logs_equivalent") &&
         Check(result.summary_equivalent, "summary_equivalent");
}

bool test_random_seed_mismatch_breaks_equivalence() {
  FakeSessionRepo repo;
  FakeSummaryReader summaries;
  FakeAgentLogReader logs;
  repo.sessions["baseline"] = MakeSession(
      "baseline", R"([{"symbol":"BTCUSDT"}])", MakeSnapshotJson(1, 7, 0.01));
  repo.sessions["rerun"] = MakeSession(
      "rerun", R"([{"symbol":"BTCUSDT"}])", MakeSnapshotJson(1, 8, 0.01));
  summaries.summaries["baseline"] =
      MakeSummary("baseline", 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0);
  summaries.summaries["rerun"] =
      MakeSummary("rerun", 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0);
  logs.logs["baseline"] = {MakeLog("baseline", 0, "b1", 1.0, 0.0, 1.0, 1, 0.0, 0.0)};
  logs.logs["rerun"] = {MakeLog("rerun", 0, "b1", 1.0, 0.0, 1.0, 1, 0.0, 0.0)};

  auto uc = BuildUseCase(&repo, &summaries, &logs);
  const auto result = uc.Run({"baseline", "rerun"});

  return Check(result.ok, "seed mismatch still produces result") &&
         Check(!result.equivalent, "seed mismatch must break equivalence") &&
         Check(!result.same_random_seed, "same_random_seed=false") &&
         Check(result.error_code == "not_equivalent", "not_equivalent code");
}

bool test_snapshot_version_mismatch_breaks_equivalence() {
  FakeSessionRepo repo;
  FakeSummaryReader summaries;
  FakeAgentLogReader logs;
  repo.sessions["baseline"] = MakeSession(
      "baseline", R"([{"symbol":"BTCUSDT"}])", MakeSnapshotJson(1, 7, 0.01));
  repo.sessions["rerun"] = MakeSession(
      "rerun", R"([{"symbol":"BTCUSDT"}])", MakeSnapshotJson(2, 7, 0.01));
  summaries.summaries["baseline"] =
      MakeSummary("baseline", 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0);
  summaries.summaries["rerun"] =
      MakeSummary("rerun", 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0);
  logs.logs["baseline"] = {MakeLog("baseline", 0, "b1", 1.0, 0.0, 1.0, 1, 0.0, 0.0)};
  logs.logs["rerun"] = {MakeLog("rerun", 0, "b1", 1.0, 0.0, 1.0, 1, 0.0, 0.0)};

  auto uc = BuildUseCase(&repo, &summaries, &logs);
  const auto result = uc.Run({"baseline", "rerun"});

  return Check(result.ok, "snapshot_version mismatch still produces result") &&
         Check(!result.equivalent, "snapshot_version mismatch must break equivalence") &&
         Check(!result.same_snapshot_version, "same_snapshot_version=false");
}

bool test_batch_order_mismatch_breaks_equivalence() {
  FakeSessionRepo repo;
  FakeSummaryReader summaries;
  FakeAgentLogReader logs;
  const std::string snapshot = MakeSnapshotJson(1, 7, 0.01);
  repo.sessions["baseline"] = MakeSession("baseline", R"([{"symbol":"BTCUSDT"}])", snapshot);
  repo.sessions["rerun"] = MakeSession("rerun", R"([{"symbol":"BTCUSDT"}])", snapshot);
  summaries.summaries["baseline"] =
      MakeSummary("baseline", 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0);
  summaries.summaries["rerun"] =
      MakeSummary("rerun", 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0);
  logs.logs["baseline"] = {
      MakeLog("baseline", 0, "b1", 1.0, 0.0, 1.0, 1, 0.0, 0.0),
      MakeLog("baseline", 1, "b2", 1.0, 0.0, 1.0, 1, 0.0, 0.0),
  };
  logs.logs["rerun"] = {
      MakeLog("rerun", 0, "b2", 1.0, 0.0, 1.0, 1, 0.0, 0.0),
      MakeLog("rerun", 1, "b1", 1.0, 0.0, 1.0, 1, 0.0, 0.0),
  };

  auto uc = BuildUseCase(&repo, &summaries, &logs);
  const auto result = uc.Run({"baseline", "rerun"});

  return Check(result.ok, "batch-order mismatch still produces result") &&
         Check(!result.equivalent, "batch-order mismatch must break equivalence") &&
         Check(!result.same_batch_order, "same_batch_order=false") &&
         Check(!result.agent_logs_equivalent, "agent_logs_equivalent=false");
}

bool test_summary_drift_outside_tolerance_breaks_equivalence() {
  FakeSessionRepo repo;
  FakeSummaryReader summaries;
  FakeAgentLogReader logs;
  const std::string snapshot = MakeSnapshotJson(1, 7, 0.001);
  repo.sessions["baseline"] = MakeSession("baseline", R"([{"symbol":"BTCUSDT"}])", snapshot);
  repo.sessions["rerun"] = MakeSession("rerun", R"([{"symbol":"BTCUSDT"}])", snapshot);
  summaries.summaries["baseline"] =
      MakeSummary("baseline", 100.0, 50.0, -1.0, 10.0, 5.0, 0.9, 10.0, 3.0, 50010.0);
  summaries.summaries["rerun"] =
      MakeSummary("rerun", 100.5, 50.0, -1.0, 10.0, 5.0, 0.9, 10.0, 3.0, 50010.0);
  logs.logs["baseline"] = {
      MakeLog("baseline", 0, "b1", 10.0, -1.0, 0.9, 1, 0.0, 0.0),
      MakeLog("baseline", 1, "b2", 90.0, -1.0, 0.9, 1, 0.0, 0.0),
  };
  logs.logs["rerun"] = {
      MakeLog("rerun", 0, "b1", 10.0, -1.0, 0.9, 1, 0.0, 0.0),
      MakeLog("rerun", 1, "b2", 90.0, -1.0, 0.9, 1, 0.0, 0.0),
  };

  auto uc = BuildUseCase(&repo, &summaries, &logs);
  const auto result = uc.Run({"baseline", "rerun"});

  return Check(result.ok, "summary drift still produces result") &&
         Check(!result.equivalent, "summary drift must break equivalence") &&
         Check(!result.summary_equivalent, "summary_equivalent=false");
}

bool test_strategy_mismatch_marks_inputs_as_different() {
  FakeSessionRepo repo;
  FakeSummaryReader summaries;
  FakeAgentLogReader logs;
  const std::string snapshot = MakeSnapshotJson(1, 7, 0.001);
  repo.sessions["baseline"] = MakeSession("baseline", R"([{"symbol":"BTCUSDT"}])", snapshot);
  repo.sessions["rerun"] = MakeSession("rerun", R"([{"symbol":"ETHUSDT"}])", snapshot);
  summaries.summaries["baseline"] =
      MakeSummary("baseline", 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0);
  summaries.summaries["rerun"] =
      MakeSummary("rerun", 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0);
  logs.logs["baseline"] = {MakeLog("baseline", 0, "b1", 1.0, 0.0, 1.0, 1, 0.0, 0.0)};
  logs.logs["rerun"] = {MakeLog("rerun", 0, "b1", 1.0, 0.0, 1.0, 1, 0.0, 0.0)};

  auto uc = BuildUseCase(&repo, &summaries, &logs);
  const auto result = uc.Run({"baseline", "rerun"});

  return Check(result.ok, "strategy mismatch still produces result") &&
         Check(!result.same_inputs, "same_inputs=false") &&
         Check(!result.same_strategy, "same_strategy=false") &&
         Check(!result.equivalent, "different inputs cannot be equivalent");
}

bool test_malformed_snapshot_json_fails_validation() {
  FakeSessionRepo repo;
  FakeSummaryReader summaries;
  FakeAgentLogReader logs;
  repo.sessions["baseline"] = MakeSession(
      "baseline", R"([{"symbol":"BTCUSDT"}])", R"({"snapshot_version":1})");
  repo.sessions["rerun"] = MakeSession(
      "rerun", R"([{"symbol":"BTCUSDT"}])", R"({"snapshot_version":1,"random_seed":7})");
  summaries.summaries["baseline"] =
      MakeSummary("baseline", 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0);
  summaries.summaries["rerun"] =
      MakeSummary("rerun", 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0);
  logs.logs["baseline"] = {MakeLog("baseline", 0, "b1", 1.0, 0.0, 1.0, 1, 0.0, 0.0)};
  logs.logs["rerun"] = {MakeLog("rerun", 0, "b1", 1.0, 0.0, 1.0, 1, 0.0, 0.0)};

  auto uc = BuildUseCase(&repo, &summaries, &logs);
  const auto result = uc.Run({"baseline", "rerun"});

  return Check(!result.ok, "malformed snapshot must fail check") &&
         Check(result.error_code == "no_data", "no_data code expected");
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_identical_rerun_within_tolerance_is_equivalent() && ok;
  ok = test_random_seed_mismatch_breaks_equivalence() && ok;
  ok = test_snapshot_version_mismatch_breaks_equivalence() && ok;
  ok = test_batch_order_mismatch_breaks_equivalence() && ok;
  ok = test_summary_drift_outside_tolerance_breaks_equivalence() && ok;
  ok = test_strategy_mismatch_marks_inputs_as_different() && ok;
  ok = test_malformed_snapshot_json_fails_validation() && ok;
  if (ok) {
    std::cout << "[OK] backtest_check_replay_determinism_test passed" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
