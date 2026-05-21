#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app/replay_orchestration_ports.hpp"
#include "app/replay_step_journal.hpp"
#include "app/restore_state_uc.hpp"
#include "app/shadow_ledger_port.hpp"

namespace {

using cex::backtest::app::AgentLogEntry;
using cex::backtest::app::BatchOutcome;
using cex::backtest::app::IAgentLogReader;
using cex::backtest::app::IShadowLedger;
using cex::backtest::app::ReplayStepJournal;
using cex::backtest::app::ReplayStepJournalEntry;
using cex::backtest::app::RestoreState;
using cex::backtest::app::ShadowLedgerApplyRequest;
using cex::backtest::app::ShadowLedgerBatchCheckpoint;
using cex::backtest::app::ShadowLedgerNamespaceState;
using cex::backtest::app::ShadowLedgerStepState;

int g_fail_count = 0;
int g_pass_count = 0;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (cond) {                                                            \
      ++g_pass_count;                                                      \
    } else {                                                               \
      ++g_fail_count;                                                      \
      std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " " << #cond \
                << std::endl;                                              \
    }                                                                      \
  } while (false)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

class FakeShadowLedger final : public IShadowLedger {
 public:
  bool NamespaceExists(const std::string& ns) override {
    return namespaces.count(ns) != 0;
  }
  bool CreateNamespace(const ShadowLedgerNamespaceState& initial) override {
    if (namespaces.count(initial.namespace_id)) return false;
    namespaces[initial.namespace_id] = initial;
    return true;
  }
  std::optional<ShadowLedgerNamespaceState> GetNamespace(
      const std::string& ns) override {
    auto it = namespaces.find(ns);
    if (it == namespaces.end()) return std::nullopt;
    return it->second;
  }
  ShadowLedgerStepState ApplyFills(const ShadowLedgerApplyRequest&) override {
    return {};
  }
  std::optional<ShadowLedgerStepState> GetLastStep(const std::string&) override {
    return std::nullopt;
  }
  std::optional<ShadowLedgerBatchCheckpoint> GetCheckpoint(
      const std::string& ns, const std::string& batch_id) override {
    auto it = checkpoints.find(ns + "|" + batch_id);
    if (it == checkpoints.end()) return std::nullopt;
    return it->second;
  }
  bool RestoreBeforeBatch(const std::string& ns,
                          const std::string& batch_id) override {
    auto it = checkpoints.find(ns + "|" + batch_id);
    if (it == checkpoints.end()) return false;
    if (restore_should_fail) return false;
    namespaces[ns] = it->second.before_state;
    last_restored_batch = batch_id;
    return true;
  }
  bool DropNamespace(const std::string& ns) override {
    return namespaces.erase(ns) > 0;
  }

  void AddCheckpoint(const std::string& ns,
                     const std::string& batch_id,
                     ShadowLedgerNamespaceState before) {
    ShadowLedgerBatchCheckpoint cp;
    cp.namespace_id = ns;
    cp.batch_id = batch_id;
    cp.before_state = std::move(before);
    checkpoints[ns + "|" + batch_id] = cp;
  }

  std::map<std::string, ShadowLedgerNamespaceState> namespaces;
  std::map<std::string, ShadowLedgerBatchCheckpoint> checkpoints;
  bool restore_should_fail{false};
  std::string last_restored_batch;
};

class FakeAgentLogReader final : public IAgentLogReader {
 public:
  std::vector<AgentLogEntry> ReadLogsUpTo(
      const std::string& session_id, uint32_t up_to) override {
    last_session_id = session_id;
    last_up_to = up_to;
    return logs;
  }

  std::vector<AgentLogEntry> logs;
  std::string last_session_id;
  uint32_t last_up_to{0};
};

AgentLogEntry MakeLog(const std::string& session_id,
                      uint32_t batch_seq,
                      const std::string& batch_id,
                      double pnl,
                      const std::string& risk = "ok",
                      bool solver_error = false) {
  AgentLogEntry log;
  log.session_id = session_id;
  log.batch_seq = batch_seq;
  log.original_batch_id = batch_id;
  log.pnl = pnl;
  log.is_value = pnl;
  log.fill_rate = 0.95;
  log.fills_applied = 1;
  log.solve_time_ms = 5;
  log.risk_status = risk;
  log.solver_error_flag = solver_error;
  return log;
}

ReplayStepJournalEntry MakeJournalEntry(const AgentLogEntry& log) {
  ReplayStepJournalEntry e;
  e.batch_id = log.original_batch_id;
  e.agent_log = log;
  e.execution.outcome = log.solver_error_flag
                            ? BatchOutcome::kHardFailure
                        : log.risk_status == "soft" ? BatchOutcome::kSoftFailure
                                                    : BatchOutcome::kOk;
  e.execution.pnl = log.pnl;
  e.execution.is_value = log.is_value;
  e.execution.fill_rate = log.fill_rate;
  e.execution.solve_time_ms = log.solve_time_ms;
  return e;
}

ShadowLedgerNamespaceState MakeNamespace(const std::string& ns) {
  ShadowLedgerNamespaceState s;
  s.namespace_id = ns;
  s.session_id = "sess-1";
  s.tracked_user_id = "user-1";
  s.balances["USDT"] = "1000.00";
  s.positions["BTC"] = "0.5";
  return s;
}

void TestRejectsMissingDependencies() {
  std::cerr << "-- TestRejectsMissingDependencies\n";
  RestoreState::Dependencies deps;
  RestoreState uc(deps);
  RestoreState::Request req;
  req.session_id = "s";
  req.namespace_id = "ns";
  const auto r = uc.Run(req);
  EXPECT_EQ(r.status, RestoreState::Status::kInvalidArgument);
}

void TestRejectsMissingNamespace() {
  std::cerr << "-- TestRejectsMissingNamespace\n";
  FakeShadowLedger ledger;
  RestoreState::Dependencies deps;
  deps.shadow_ledger = &ledger;
  RestoreState uc(deps);
  RestoreState::Request req;
  req.session_id = "s";
  req.namespace_id = "ns-missing";
  req.target_batch_id = "b1";
  const auto r = uc.Run(req);
  EXPECT_EQ(r.status, RestoreState::Status::kNamespaceMissing);
}

void TestColdStartReturnsBaseline() {
  std::cerr << "-- TestColdStartReturnsBaseline\n";
  FakeShadowLedger ledger;
  ledger.namespaces["ns-1"] = MakeNamespace("ns-1");
  RestoreState::Dependencies deps;
  deps.shadow_ledger = &ledger;
  RestoreState uc(deps);
  RestoreState::Request req;
  req.session_id = "s";
  req.namespace_id = "ns-1";
  // empty target_batch_id => baseline
  const auto r = uc.Run(req);
  EXPECT_EQ(r.status, RestoreState::Status::kOk);
  EXPECT_EQ(r.state.balances.at("USDT"), std::string("1000.00"));
  EXPECT_EQ(r.state.positions.at("BTC"), std::string("0.5"));
  EXPECT_EQ(r.state.rolling.processed_batches, 0u);
  EXPECT(!r.state.last_action.has_value());
}

void TestWarmRestoreRecomputesRollingMetrics() {
  std::cerr << "-- TestWarmRestoreRecomputesRollingMetrics\n";
  FakeShadowLedger ledger;
  ledger.namespaces["ns-1"] = MakeNamespace("ns-1");
  ledger.AddCheckpoint("ns-1", "b3", MakeNamespace("ns-1"));

  ReplayStepJournal journal;
  journal.Upsert(MakeJournalEntry(MakeLog("s", 0, "b1", 10.0)));
  journal.Upsert(MakeJournalEntry(MakeLog("s", 1, "b2", 20.0)));
  journal.Upsert(MakeJournalEntry(MakeLog("s", 2, "b3", 5.0)));  // target

  RestoreState::Dependencies deps;
  deps.shadow_ledger = &ledger;
  deps.journal = &journal;
  RestoreState uc(deps);

  RestoreState::Request req;
  req.session_id = "s";
  req.namespace_id = "ns-1";
  req.target_batch_id = "b3";
  req.mode = RestoreState::Mode::kWarm;

  const auto r = uc.Run(req);
  EXPECT_EQ(r.status, RestoreState::Status::kOk);
  EXPECT(!r.used_cold_path);
  EXPECT_EQ(r.restored_steps, 2u);
  EXPECT_EQ(r.state.rolling.processed_batches, 2u);
  EXPECT_EQ(r.state.rolling.cum_pnl, 30.0);
  EXPECT_EQ(r.state.rolling.sum_pnl, 30.0);
  EXPECT(r.state.last_action.has_value());
  EXPECT_EQ(r.state.last_action->original_batch_id, std::string("b2"));
  EXPECT_EQ(journal.Size(), 2u);  // target b3 truncated
  EXPECT(!journal.Contains("b3"));
  EXPECT_EQ(ledger.last_restored_batch, std::string("b3"));
}

void TestWarmRestoreFailsIfNoCheckpoint() {
  std::cerr << "-- TestWarmRestoreFailsIfNoCheckpoint\n";
  FakeShadowLedger ledger;
  ledger.namespaces["ns-1"] = MakeNamespace("ns-1");

  ReplayStepJournal journal;
  journal.Upsert(MakeJournalEntry(MakeLog("s", 0, "b1", 10.0)));

  RestoreState::Dependencies deps;
  deps.shadow_ledger = &ledger;
  deps.journal = &journal;
  RestoreState uc(deps);

  RestoreState::Request req;
  req.session_id = "s";
  req.namespace_id = "ns-1";
  req.target_batch_id = "b1";
  req.mode = RestoreState::Mode::kWarm;

  const auto r = uc.Run(req);
  EXPECT_EQ(r.status, RestoreState::Status::kCheckpointMissing);
}

void TestWarmRestoreFailsIfShadowRefuses() {
  std::cerr << "-- TestWarmRestoreFailsIfShadowRefuses\n";
  FakeShadowLedger ledger;
  ledger.namespaces["ns-1"] = MakeNamespace("ns-1");
  ledger.AddCheckpoint("ns-1", "b1", MakeNamespace("ns-1"));
  ledger.restore_should_fail = true;

  ReplayStepJournal journal;
  journal.Upsert(MakeJournalEntry(MakeLog("s", 0, "b1", 10.0)));

  RestoreState::Dependencies deps;
  deps.shadow_ledger = &ledger;
  deps.journal = &journal;
  RestoreState uc(deps);

  RestoreState::Request req;
  req.session_id = "s";
  req.namespace_id = "ns-1";
  req.target_batch_id = "b1";
  req.mode = RestoreState::Mode::kWarm;

  const auto r = uc.Run(req);
  EXPECT_EQ(r.status, RestoreState::Status::kShadowRestoreFailed);
}

void TestColdRestoreReadsAgentLogsAndHydratesJournal() {
  std::cerr << "-- TestColdRestoreReadsAgentLogsAndHydratesJournal\n";
  FakeShadowLedger ledger;
  ledger.namespaces["ns-1"] = MakeNamespace("ns-1");
  ledger.AddCheckpoint("ns-1", "b3", MakeNamespace("ns-1"));

  FakeAgentLogReader reader;
  reader.logs = {
      MakeLog("s", 0, "b1", 10.0),
      MakeLog("s", 1, "b2", 20.0, "soft"),  // counts as failed step
      MakeLog("s", 2, "b3", 5.0),           // must be dropped (target)
  };

  ReplayStepJournal journal;  // empty: cold path

  RestoreState::Dependencies deps;
  deps.shadow_ledger = &ledger;
  deps.journal = &journal;
  deps.agent_log_reader = &reader;
  RestoreState uc(deps);

  RestoreState::Request req;
  req.session_id = "s";
  req.namespace_id = "ns-1";
  req.target_batch_id = "b3";
  req.target_batch_seq = 2u;
  req.mode = RestoreState::Mode::kAuto;  // journal empty -> cold

  const auto r = uc.Run(req);
  EXPECT_EQ(r.status, RestoreState::Status::kOk);
  EXPECT(r.used_cold_path);
  EXPECT_EQ(r.restored_steps, 2u);
  EXPECT_EQ(r.state.rolling.processed_batches, 1u);
  EXPECT_EQ(r.state.rolling.failed_batches, 1u);
  EXPECT_EQ(r.state.rolling.cum_pnl, 10.0);
  EXPECT_EQ(r.state.risk_alerts_count, 1u);
  EXPECT_EQ(journal.Size(), 2u);
  EXPECT(journal.Contains("b1"));
  EXPECT(journal.Contains("b2"));
  EXPECT(!journal.Contains("b3"));
  EXPECT_EQ(reader.last_session_id, std::string("s"));
  EXPECT_EQ(reader.last_up_to, 2u);
}

void TestColdRestoreFailsWithoutReader() {
  std::cerr << "-- TestColdRestoreFailsWithoutReader\n";
  FakeShadowLedger ledger;
  ledger.namespaces["ns-1"] = MakeNamespace("ns-1");
  ledger.AddCheckpoint("ns-1", "b3", MakeNamespace("ns-1"));

  ReplayStepJournal journal;  // empty -> cold
  RestoreState::Dependencies deps;
  deps.shadow_ledger = &ledger;
  deps.journal = &journal;
  RestoreState uc(deps);

  RestoreState::Request req;
  req.session_id = "s";
  req.namespace_id = "ns-1";
  req.target_batch_id = "b3";
  req.mode = RestoreState::Mode::kCold;

  const auto r = uc.Run(req);
  EXPECT_EQ(r.status, RestoreState::Status::kJournalMissing);
}

void TestAutoModePicksWarmWhenJournalHasTarget() {
  std::cerr << "-- TestAutoModePicksWarmWhenJournalHasTarget\n";
  FakeShadowLedger ledger;
  ledger.namespaces["ns-1"] = MakeNamespace("ns-1");
  ledger.AddCheckpoint("ns-1", "b2", MakeNamespace("ns-1"));

  ReplayStepJournal journal;
  journal.Upsert(MakeJournalEntry(MakeLog("s", 0, "b1", 10.0)));
  journal.Upsert(MakeJournalEntry(MakeLog("s", 1, "b2", 5.0)));

  // Reader present but should not be consulted on the warm path.
  FakeAgentLogReader reader;
  reader.logs = {MakeLog("s", 99, "should-not-be-used", 999.0)};

  RestoreState::Dependencies deps;
  deps.shadow_ledger = &ledger;
  deps.journal = &journal;
  deps.agent_log_reader = &reader;
  RestoreState uc(deps);

  RestoreState::Request req;
  req.session_id = "s";
  req.namespace_id = "ns-1";
  req.target_batch_id = "b2";
  req.mode = RestoreState::Mode::kAuto;

  const auto r = uc.Run(req);
  EXPECT_EQ(r.status, RestoreState::Status::kOk);
  EXPECT(!r.used_cold_path);
  EXPECT_EQ(r.restored_steps, 1u);
  EXPECT_EQ(r.state.rolling.cum_pnl, 10.0);
  EXPECT_EQ(reader.last_session_id, std::string(""));  // never called
}

void TestDrawdownTracking() {
  std::cerr << "-- TestDrawdownTracking\n";
  FakeShadowLedger ledger;
  ledger.namespaces["ns-1"] = MakeNamespace("ns-1");
  ledger.AddCheckpoint("ns-1", "bX", MakeNamespace("ns-1"));

  ReplayStepJournal journal;
  // pnls: +30, -10, -15, +5  -> peak=30, trough=5, max_drawdown=25
  journal.Upsert(MakeJournalEntry(MakeLog("s", 0, "b1", 30.0)));
  journal.Upsert(MakeJournalEntry(MakeLog("s", 1, "b2", -10.0)));
  journal.Upsert(MakeJournalEntry(MakeLog("s", 2, "b3", -15.0)));
  journal.Upsert(MakeJournalEntry(MakeLog("s", 3, "b4", 5.0)));
  journal.Upsert(MakeJournalEntry(MakeLog("s", 4, "bX", 1.0)));  // target

  RestoreState::Dependencies deps;
  deps.shadow_ledger = &ledger;
  deps.journal = &journal;
  RestoreState uc(deps);

  RestoreState::Request req;
  req.session_id = "s";
  req.namespace_id = "ns-1";
  req.target_batch_id = "bX";
  req.mode = RestoreState::Mode::kWarm;

  const auto r = uc.Run(req);
  EXPECT_EQ(r.status, RestoreState::Status::kOk);
  EXPECT_EQ(r.state.rolling.cum_pnl, 10.0);  // 30-10-15+5
  EXPECT_EQ(r.state.rolling.peak_cum_pnl, 30.0);
  EXPECT_EQ(r.state.rolling.max_drawdown, 25.0);
}

}  // namespace

int main() {
  TestRejectsMissingDependencies();
  TestRejectsMissingNamespace();
  TestColdStartReturnsBaseline();
  TestWarmRestoreRecomputesRollingMetrics();
  TestWarmRestoreFailsIfNoCheckpoint();
  TestWarmRestoreFailsIfShadowRefuses();
  TestColdRestoreReadsAgentLogsAndHydratesJournal();
  TestColdRestoreFailsWithoutReader();
  TestAutoModePicksWarmWhenJournalHasTarget();
  TestDrawdownTracking();

  std::cerr << "\nPassed: " << g_pass_count << ", Failed: " << g_fail_count
            << std::endl;
  return g_fail_count == 0 ? 0 : 1;
}
