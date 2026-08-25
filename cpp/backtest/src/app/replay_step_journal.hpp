#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/replay_orchestration_ports.hpp"

namespace cex::backtest::app {

struct ReplayStepJournalEntry {
  std::string batch_id;
  AgentLogEntry agent_log;
  BatchExecutionResult execution;
};

// In-memory step journal for one replay session.
//
// The journal is append-ordered by first-seen batch_id, but supports replacing
// an existing batch entry in place. This lets replay retry / restart logic
// recompute AgentLog and ReplaySummary without double-counting a re-run step.
class ReplayStepJournal {
 public:
  std::size_t Size() const;
  bool Empty() const;
  bool Contains(const std::string& batch_id) const;
  uint32_t NextBatchSeq() const;

  void Upsert(ReplayStepJournalEntry entry);
  bool TruncateFromBatch(const std::string& batch_id);

  std::optional<ReplayStepJournalEntry> Find(const std::string& batch_id) const;
  std::vector<ReplayStepJournalEntry> OrderedEntries() const;

  ReplaySummary BuildSummary(const std::string& session_id,
                             uint32_t total_batches,
                             const std::string& avgis_rule = "volume_weighted",
                             const std::string& decision_price_source =
                                 "marketdata_mid_with_clearprice_fallback") const;

 private:
  std::vector<std::string> order_;
  std::unordered_map<std::string, ReplayStepJournalEntry> entries_;
};

}  // namespace cex::backtest::app
