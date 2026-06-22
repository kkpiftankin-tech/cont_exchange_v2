#pragma once

#include "app/replay_orchestration_ports.hpp"
#include "infra/ephemeral_session_registry.hpp"

namespace cex::backtest::infra {

// Implements IAgentLogWriter with ephemeral routing.
//
//   ephemeral session  →  WriteAgentLog is a no-op (F15-PERSIST-1: nothing
//                         written to ClickHouse replay_agentlogs)
//   persistent session →  delegate to the wrapped writer
//
// The agent_log_reader (ClickHouse) is NOT wrapped; it stays pointed at the
// real ClickHouse storage because read paths are irrelevant for ephemeral
// sessions (the UI will get 404/nodata for ephemeral ids anyway).
class RoutingAgentLogWriter final : public app::IAgentLogWriter {
 public:
  // delegate may be nullptr when ClickHouse is unavailable.
  explicit RoutingAgentLogWriter(app::IAgentLogWriter* delegate,
                                 EphemeralSessionRegistry* registry);

  void WriteAgentLog(const app::AgentLogEntry& entry) override;

 private:
  app::IAgentLogWriter* delegate_;
  EphemeralSessionRegistry* registry_;
};

}  // namespace cex::backtest::infra
