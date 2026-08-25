#include "infra/routing_agent_log_writer.hpp"

namespace cex::backtest::infra {

RoutingAgentLogWriter::RoutingAgentLogWriter(app::IAgentLogWriter* delegate,
                                             EphemeralSessionRegistry* registry)
    : delegate_(delegate), registry_(registry) {}

void RoutingAgentLogWriter::WriteAgentLog(const app::AgentLogEntry& entry) {
  // F15-PERSIST-1: ephemeral sessions produce no ClickHouse rows.
  if (registry_->IsEphemeral(entry.session_id)) return;
  if (delegate_ == nullptr) return;
  delegate_->WriteAgentLog(entry);
}

}  // namespace cex::backtest::infra
