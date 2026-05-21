#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "app/replay_orchestration_ports.hpp"
#include "app/replay_step_journal.hpp"

namespace {

using cex::backtest::app::AgentLogEntry;
using cex::backtest::app::BatchExecutionResult;
using cex::backtest::app::BatchOutcome;
using cex::backtest::app::IReplaySummaryStore;
using cex::backtest::app::ReplayStepJournal;
using cex::backtest::app::ReplayStepJournalEntry;
using cex::backtest::app::ReplaySummary;

int g_pass = 0;
int g_fail = 0;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (cond) {                                                            \
      ++g_pass;                                                            \
    } else {                                                               \
      ++g_fail;                                                            \
      std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " " << #cond \
                << std::endl;                                              \
    }                                                                      \
  } while (false)

#define EXPECT_NEAR(a, b, eps) EXPECT(std::abs((a) - (b)) < (eps))

ReplayStepJournalEntry MakeEntry(const std::string& session_id,
                                 const std::string& batch_id,
                                 uint32_t batch_seq,
                                 BatchOutcome outcome,
                                 double pnl,
                                 double is_value = 0.0,
                                 double fill_rate = 0.0,
                                 uint32_t solve_time_ms = 0,
                                 double vwap = 0.0,
                                 double executed_qty = 0.0,
                                 double requested_qty = 0.0,
                                 double executed_notional = 0.0,
                                 double is_weighted_sum = 0.0,
                                 double is_weight = 0.0,
                                 uint32_t fills_applied = 0) {
  ReplayStepJournalEntry e;
  e.batch_id = batch_id;
  e.agent_log.session_id = session_id;
  e.agent_log.original_batch_id = batch_id;
  e.agent_log.batch_seq = batch_seq;
  e.agent_log.pnl = pnl;
  e.execution.outcome = outcome;
  e.execution.pnl = pnl;
  e.execution.is_value = is_value;
  e.execution.fill_rate = fill_rate;
  e.execution.solve_time_ms = solve_time_ms;
  e.execution.vwap = vwap;
  e.execution.executed_qty = executed_qty;
  e.execution.requested_qty = requested_qty;
  e.execution.executed_notional = executed_notional;
  e.execution.is_weighted_sum = is_weighted_sum;
  e.execution.is_weight = is_weight;
  e.execution.fills_applied = fills_applied;
  return e;
}

void TestEmptyJournalProducesZeroedSummary() {
  std::cerr << "-- TestEmptyJournalProducesZeroedSummary\n";
  ReplayStepJournal journal;
  const auto s = journal.BuildSummary("sess-empty", 0);
  EXPECT(s.session_id == "sess-empty");
  EXPECT(s.total_batches == 0);
  EXPECT(s.processed_batches == 0);
  EXPECT(s.failed_batches == 0);
  EXPECT_NEAR(s.total_pnl, 0.0, 1e-12);
  EXPECT_NEAR(s.avg_vwap, 0.0, 1e-12);
  EXPECT_NEAR(s.sharpe, 0.0, 1e-12);
}

void TestAvgVwapZeroWhenTotalExecQtyIsZero() {
  std::cerr << "-- TestAvgVwapZeroWhenTotalExecQtyIsZero\n";
  ReplayStepJournal j;
  j.Upsert(MakeEntry("s", "b1", 0, BatchOutcome::kOk,        10.0, 0, 0, 0, 100.0));
  j.Upsert(MakeEntry("s", "b2", 1, BatchOutcome::kOk,        20.0, 0, 0, 0,   0.0));  // no vwap
  j.Upsert(MakeEntry("s", "b3", 2, BatchOutcome::kSoftFailure, 0.0, 0, 0, 0, 999.0)); // failed -> ignored
  j.Upsert(MakeEntry("s", "b4", 3, BatchOutcome::kOk,        30.0, 0, 0, 0, 200.0));

  const auto s = j.BuildSummary("s", 4);
  EXPECT(s.processed_batches == 3);
  EXPECT(s.failed_batches == 1);
  // F-15: VWAP is sum(execqty*execprice)/sum(execqty); zero execqty -> 0.
  EXPECT_NEAR(s.avg_vwap, 0.0, 1e-9);
}

void TestSharpeAndStdPnl() {
  std::cerr << "-- TestSharpeAndStdPnl\n";
  ReplayStepJournal j;
  // pnls: 10, 20, 30 (population variance) -> mean=20, var=66.6667, std≈8.165
  j.Upsert(MakeEntry("s", "b1", 0, BatchOutcome::kOk, 10.0));
  j.Upsert(MakeEntry("s", "b2", 1, BatchOutcome::kOk, 20.0));
  j.Upsert(MakeEntry("s", "b3", 2, BatchOutcome::kOk, 30.0));

  const auto s = j.BuildSummary("s", 3);
  EXPECT_NEAR(s.avg_pnl, 20.0, 1e-9);
  EXPECT_NEAR(s.total_pnl, 60.0, 1e-9);
  EXPECT_NEAR(s.std_pnl, std::sqrt(200.0 / 3.0), 1e-9);
  EXPECT_NEAR(s.sharpe, 20.0 / s.std_pnl, 1e-9);
}

void TestSharpeZeroWhenZeroVariance() {
  std::cerr << "-- TestSharpeZeroWhenZeroVariance\n";
  ReplayStepJournal j;
  j.Upsert(MakeEntry("s", "b1", 0, BatchOutcome::kOk, 5.0));
  j.Upsert(MakeEntry("s", "b2", 1, BatchOutcome::kOk, 5.0));
  const auto s = j.BuildSummary("s", 2);
  EXPECT_NEAR(s.std_pnl, 0.0, 1e-12);
  EXPECT_NEAR(s.sharpe, 0.0, 1e-12);
}

void TestMaxDrawdownTracksPeakToTrough() {
  std::cerr << "-- TestMaxDrawdownTracksPeakToTrough\n";
  ReplayStepJournal j;
  // cum: +30, 20, 5, 10  -> peak=30, trough=5, drawdown=25
  j.Upsert(MakeEntry("s", "b1", 0, BatchOutcome::kOk, 30.0));
  j.Upsert(MakeEntry("s", "b2", 1, BatchOutcome::kOk, -10.0));
  j.Upsert(MakeEntry("s", "b3", 2, BatchOutcome::kOk, -15.0));
  j.Upsert(MakeEntry("s", "b4", 3, BatchOutcome::kOk, 5.0));

  const auto s = j.BuildSummary("s", 4);
  EXPECT_NEAR(s.max_drawdown, 25.0, 1e-9);
  EXPECT_NEAR(s.total_pnl, 10.0, 1e-9);
}

void TestProcessedAndFailedCounts() {
  std::cerr << "-- TestProcessedAndFailedCounts\n";
  ReplayStepJournal j;
  j.Upsert(MakeEntry("s", "b1", 0, BatchOutcome::kOk,          1.0));
  j.Upsert(MakeEntry("s", "b2", 1, BatchOutcome::kSoftFailure, 0.0));
  j.Upsert(MakeEntry("s", "b3", 2, BatchOutcome::kHardFailure, 0.0));
  j.Upsert(MakeEntry("s", "b4", 3, BatchOutcome::kOk,          2.0));

  const auto s = j.BuildSummary("s", 4);
  EXPECT(s.processed_batches == 2);
  EXPECT(s.failed_batches == 2);
  // partial is set by RunReplaySession, not BuildSummary; it stays default false here.
  EXPECT(!s.partial);
}

void TestF15PnlMetricsUsePersistedAgentLogsIncludingFailures() {
  std::cerr << "-- TestF15PnlMetricsUsePersistedAgentLogsIncludingFailures\n";
  ReplayStepJournal j;
  j.Upsert(MakeEntry("s", "b1", 0, BatchOutcome::kOk, 10.0));
  j.Upsert(MakeEntry("s", "b2", 1, BatchOutcome::kSoftFailure, -5.0));
  j.Upsert(MakeEntry("s", "b3", 2, BatchOutcome::kHardFailure, 2.0));

  const auto s = j.BuildSummary("s", 3);
  EXPECT(s.processed_batches == 1);
  EXPECT(s.failed_batches == 2);
  EXPECT_NEAR(s.total_pnl, 7.0, 1e-9);
  EXPECT_NEAR(s.avg_pnl, 7.0 / 3.0, 1e-9);
  EXPECT_NEAR(s.max_drawdown, 5.0, 1e-9);
}

void TestAvgFillRateAndSolveTimeUsesProcessedOnly() {
  std::cerr << "-- TestAvgFillRateAndSolveTimeUsesProcessedOnly\n";
  ReplayStepJournal j;
  j.Upsert(MakeEntry("s", "b1", 0, BatchOutcome::kOk, 0.0, 0, 0.5, 10));
  j.Upsert(MakeEntry("s", "b2", 1, BatchOutcome::kSoftFailure, 0.0, 0, 0.99, 999));
  j.Upsert(MakeEntry("s", "b3", 2, BatchOutcome::kOk, 0.0, 0, 0.7, 20));

  const auto s = j.BuildSummary("s", 3);
  // F-15: if sum(Qmax/requested_qty) is zero, FillRate is zero.
  EXPECT_NEAR(s.avg_fill_rate, 0.0, 1e-9);
  EXPECT_NEAR(s.avg_solve_time_ms, 15.0, 1e-9);
}

void TestF15FillRateUsesExecutedQtyOverQmaxWhenAvailable() {
  std::cerr << "-- TestF15FillRateUsesExecutedQtyOverQmaxWhenAvailable\n";
  ReplayStepJournal j;
  j.Upsert(MakeEntry("s", "b1", 0, BatchOutcome::kOk, 0.0, 0, 10.0, 10, 0,
                     40.0, 100.0, 0.0, 0.0, 0.0, 2));
  j.Upsert(MakeEntry("s", "b2", 1, BatchOutcome::kOk, 0.0, 0, 90.0, 20, 0,
                     10.0, 100.0, 0.0, 0.0, 0.0, 3));

  const auto s = j.BuildSummary("s", 2);
  EXPECT_NEAR(s.avg_fill_rate, 25.0, 1e-9);  // (40 + 10) / (100 + 100) * 100
  EXPECT(s.total_fill_events == 5);
}

void TestF15GlobalVwapUsesNotionalOverExecutedQty() {
  std::cerr << "-- TestF15GlobalVwapUsesNotionalOverExecutedQty\n";
  ReplayStepJournal j;
  j.Upsert(MakeEntry("s", "b1", 0, BatchOutcome::kOk, 0.0, 0, 0, 0, 111,
                     2.0, 0.0, 200.0));
  j.Upsert(MakeEntry("s", "b2", 1, BatchOutcome::kOk, 0.0, 0, 0, 0, 999,
                     3.0, 0.0, 450.0));

  const auto s = j.BuildSummary("s", 2);
  EXPECT_NEAR(s.avg_vwap, 130.0, 1e-9);  // (200 + 450) / (2 + 3)
}

void TestF15AvgIsVolumeWeightedWhenWeightsAvailable() {
  std::cerr << "-- TestF15AvgIsVolumeWeightedWhenWeightsAvailable\n";
  ReplayStepJournal j;
  j.Upsert(MakeEntry("s", "buy", 0, BatchOutcome::kOk, 0.0, 50.0, 0, 0, 0,
                     1.0, 1.0, 60100.0, 50.0, 1.0));
  j.Upsert(MakeEntry("s", "sell", 1, BatchOutcome::kOk, 0.0, -150.0, 0, 0, 0,
                     3.0, 3.0, 179700.0, -450.0, 3.0));

  const auto s = j.BuildSummary("s", 2);
  EXPECT_NEAR(s.avg_is, -100.0, 1e-9);  // (50*1 + -150*3) / 4
  EXPECT(s.avgis_rule == "volume_weighted");
  EXPECT(s.decision_price_source == "marketdata_mid_with_clearprice_fallback");
}

void TestF15AvgIsSimpleMeanWhenPersistedRuleSaysSo() {
  std::cerr << "-- TestF15AvgIsSimpleMeanWhenPersistedRuleSaysSo\n";
  ReplayStepJournal j;
  j.Upsert(MakeEntry("s", "buy", 0, BatchOutcome::kOk, 0.0, 50.0, 0, 0, 0,
                     1.0, 1.0, 60100.0, 50.0, 1.0));
  j.Upsert(MakeEntry("s", "sell", 1, BatchOutcome::kOk, 0.0, -150.0, 0, 0, 0,
                     3.0, 3.0, 179700.0, -450.0, 3.0));

  const auto s = j.BuildSummary(
      "s", 2, "simple_mean", "marketdata_mid_with_clearprice_fallback");
  EXPECT_NEAR(s.avg_is, -50.0, 1e-9);  // (50 + -150) / 2
  EXPECT(s.avgis_rule == "simple_mean");
}

void TestF15ZeroRequestedVolumeDiagnostic() {
  std::cerr << "-- TestF15ZeroRequestedVolumeDiagnostic\n";
  ReplayStepJournal j;
  j.Upsert(MakeEntry("s", "b1", 0, BatchOutcome::kOk, 0.0, 0, 0, 1, 0,
                     0.0, 0.0, 0.0));

  const auto s = j.BuildSummary("s", 1);
  EXPECT_NEAR(s.avg_fill_rate, 0.0, 1e-9);
  EXPECT(s.no_requested_volume);
}

// Fake summary store mirrors what RunReplaySession will call. Validates the
// SaveSummary contract: idempotent upsert keyed by session_id.
class FakeSummaryStore final : public IReplaySummaryStore {
 public:
  void SaveSummary(const ReplaySummary& s) override { writes.push_back(s); }
  std::vector<ReplaySummary> writes;
};

void TestSaveSummaryIsCallableAndIdempotent() {
  std::cerr << "-- TestSaveSummaryIsCallableAndIdempotent\n";
  FakeSummaryStore store;

  ReplayStepJournal j;
  j.Upsert(MakeEntry("sess-1", "b1", 0, BatchOutcome::kOk, 10.0));
  const auto first = j.BuildSummary("sess-1", 1);

  store.SaveSummary(first);
  // Re-aggregate after a re-run that adjusts the batch payload.
  j.Upsert(MakeEntry("sess-1", "b1", 0, BatchOutcome::kOk, 99.0));
  const auto rerun = j.BuildSummary("sess-1", 1);
  store.SaveSummary(rerun);

  EXPECT(store.writes.size() == 2);
  EXPECT(store.writes.front().total_pnl == 10.0);
  EXPECT(store.writes.back().total_pnl == 99.0);
  // session_id stays stable -> downstream PG upsert collapses to one row.
  EXPECT(store.writes.front().session_id == store.writes.back().session_id);
}

}  // namespace

int main() {
  TestEmptyJournalProducesZeroedSummary();
  TestAvgVwapZeroWhenTotalExecQtyIsZero();
  TestSharpeAndStdPnl();
  TestSharpeZeroWhenZeroVariance();
  TestMaxDrawdownTracksPeakToTrough();
  TestProcessedAndFailedCounts();
  TestF15PnlMetricsUsePersistedAgentLogsIncludingFailures();
  TestAvgFillRateAndSolveTimeUsesProcessedOnly();
  TestF15FillRateUsesExecutedQtyOverQmaxWhenAvailable();
  TestF15GlobalVwapUsesNotionalOverExecutedQty();
  TestF15AvgIsVolumeWeightedWhenWeightsAvailable();
  TestF15AvgIsSimpleMeanWhenPersistedRuleSaysSo();
  TestF15ZeroRequestedVolumeDiagnostic();
  TestSaveSummaryIsCallableAndIdempotent();

  std::cerr << "\nPassed: " << g_pass << ", Failed: " << g_fail << std::endl;
  return g_fail == 0 ? 0 : 1;
}
