#pragma once

#include <optional>
#include <string>
#include <vector>

#include "app/replay_orchestration_ports.hpp"

namespace cex::backtest::app {

// Minimal batch-order slice needed by F15-METRIC-4 compatibility checks.
struct ReplayAgentLogRef {
  uint32_t batch_seq{0};
  std::string original_batch_id;
};

class IReplaySummaryReader {
 public:
  virtual ~IReplaySummaryReader() = default;
  virtual std::optional<ReplaySummary> GetSummaryBySessionId(
      const std::string& session_id) = 0;
};

class IReplayAgentLogReader {
 public:
  virtual ~IReplayAgentLogReader() = default;
  virtual std::vector<ReplayAgentLogRef> LoadAgentLogRefsBySessionId(
      const std::string& session_id) = 0;
};

}  // namespace cex::backtest::app
