#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "app/replay_step_journal.hpp"

namespace {

using cex::backtest::app::AgentLogEntry;
using cex::backtest::app::BatchExecutionResult;
using cex::backtest::app::BatchOutcome;
using cex::backtest::app::ReplayStepJournal;
using cex::backtest::app::ReplayStepJournalEntry;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

ReplayStepJournalEntry MakeEntry(const std::string& session_id,
                                 const std::string& batch_id,
                                 uint32_t batch_seq,
                                 BatchOutcome outcome,
                                 double pnl,
                                 double is_value = 0.0,
                                 double fill_rate = 0.0,
                                 uint32_t solve_time_ms = 0) {
  ReplayStepJournalEntry entry;
  entry.batch_id = batch_id;
  entry.agent_log.session_id = session_id;
  entry.agent_log.original_batch_id = batch_id;
  entry.agent_log.batch_seq = batch_seq;
  entry.agent_log.pnl = pnl;
  entry.execution.outcome = outcome;
  entry.execution.pnl = pnl;
  entry.execution.is_value = is_value;
  entry.execution.fill_rate = fill_rate;
  entry.execution.solve_time_ms = solve_time_ms;
  return entry;
}

bool test_upsert_replaces_existing_batch() {
  ReplayStepJournal journal;
  journal.Upsert(MakeEntry("sess-1", "b1", 0, BatchOutcome::kOk, 10.0));
  journal.Upsert(MakeEntry("sess-1", "b1", 0, BatchOutcome::kOk, 42.0));

  if (!Check(journal.Size() == 1, "same batch must not duplicate order")) return false;
  const auto entry = journal.Find("b1");
  if (!Check(entry.has_value(), "entry must exist")) return false;
  return Check(std::abs(entry->execution.pnl - 42.0) < 1e-9,
               "replacement payload must win");
}

bool test_truncate_from_batch_discards_tail() {
  ReplayStepJournal journal;
  journal.Upsert(MakeEntry("sess-1", "b1", 0, BatchOutcome::kOk, 10.0));
  journal.Upsert(MakeEntry("sess-1", "b2", 1, BatchOutcome::kOk, 20.0));
  journal.Upsert(MakeEntry("sess-1", "b3", 2, BatchOutcome::kSoftFailure, 0.0));

  if (!Check(journal.TruncateFromBatch("b2"), "truncate must succeed")) return false;
  if (!Check(journal.Size() == 1, "tail removed")) return false;
  if (!Check(journal.Contains("b1"), "head kept")) return false;
  if (!Check(!journal.Contains("b2"), "truncate removes pivot")) return false;
  return Check(!journal.Contains("b3"), "truncate removes later batches");
}

bool test_summary_rebuild_uses_replaced_entries() {
  ReplayStepJournal journal;
  journal.Upsert(MakeEntry("sess-1", "b1", 0, BatchOutcome::kOk, 10.0, 1.0, 0.5, 5));
  journal.Upsert(MakeEntry("sess-1", "b2", 1, BatchOutcome::kSoftFailure, 0.0));
  journal.Upsert(MakeEntry("sess-1", "b3", 2, BatchOutcome::kOk, -3.0, 2.0, 0.9, 8));
  journal.Upsert(MakeEntry("sess-1", "b1", 0, BatchOutcome::kOk, 42.0, 4.0, 0.7, 6));

  const auto summary = journal.BuildSummary("sess-1", 3);
  if (!Check(summary.total_batches == 3, "total batches kept")) return false;
  if (!Check(summary.processed_batches == 2, "two ok batches")) return false;
  if (!Check(summary.failed_batches == 1, "one soft failure")) return false;
  if (!Check(std::abs(summary.total_pnl - 39.0) < 1e-9, "replacement affects total pnl"))
    return false;
  const double expected_avg = 13.0;
  const double expected_std = std::sqrt(422.0);
  if (!Check(std::abs(summary.avg_pnl - expected_avg) < 1e-9,
             "avg pnl rebuilt from all persisted AgentLogs")) return false;
  if (!Check(std::abs(summary.std_pnl - expected_std) < 1e-9,
             "std pnl rebuilt from all persisted AgentLogs")) return false;
  if (!Check(std::abs(summary.sharpe - (expected_avg / expected_std)) < 1e-9,
             "sharpe rebuilt"))
    return false;
  return Check(std::abs(summary.max_drawdown - 3.0) < 1e-9,
               "drawdown rebuilt from replaced sequence");
}

}  // namespace

int main() {
  bool all_passed = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) {
      std::cerr << "  in test: " << name << std::endl;
      all_passed = false;
    }
  };

  run("test_upsert_replaces_existing_batch", test_upsert_replaces_existing_batch);
  run("test_truncate_from_batch_discards_tail", test_truncate_from_batch_discards_tail);
  run("test_summary_rebuild_uses_replaced_entries",
      test_summary_rebuild_uses_replaced_entries);

  if (all_passed) {
    std::cout << "[OK] backtest_replay_step_journal_test passed (3 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
