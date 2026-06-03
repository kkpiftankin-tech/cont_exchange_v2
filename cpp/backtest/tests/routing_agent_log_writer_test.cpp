// Unit tests for RoutingAgentLogWriter (F15-PERSIST).
//
// Covers:
//   - ephemeral session ⇒ WriteAgentLog: delegate NOT called
//   - persistent session ⇒ WriteAgentLog: delegate called once

#include <iostream>
#include <string>

#include "app/replay_orchestration_ports.hpp"
#include "app/replay_session.hpp"
#include "infra/ephemeral_session_registry.hpp"
#include "infra/routing_agent_log_writer.hpp"

namespace {

using cex::backtest::app::AgentLogEntry;
using cex::backtest::app::IAgentLogWriter;
using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionStatus;
using cex::backtest::infra::EphemeralSessionRegistry;
using cex::backtest::infra::RoutingAgentLogWriter;

bool Check(bool condition, const std::string& msg) {
  if (condition) return true;
  std::cerr << "[FAIL] " << msg << "\n";
  return false;
}

struct SpyLogWriter final : public IAgentLogWriter {
  int write_calls{0};
  void WriteAgentLog(const AgentLogEntry&) override { ++write_calls; }
};

ReplaySession MakeEphemeralSession(const std::string& id) {
  ReplaySession s;
  s.session_id = id;
  s.persist = false;
  s.status = ReplaySessionStatus::kPending;
  return s;
}

AgentLogEntry MakeEntry(const std::string& session_id) {
  AgentLogEntry e;
  e.session_id = session_id;
  return e;
}

int main() {
  int failures = 0;

  // ---- ephemeral: WriteAgentLog is no-op ----
  {
    SpyLogWriter spy;
    EphemeralSessionRegistry reg;
    reg.Insert(MakeEphemeralSession("eph-log-1"));

    RoutingAgentLogWriter writer(&spy, &reg);
    writer.WriteAgentLog(MakeEntry("eph-log-1"));
    failures += Check(spy.write_calls == 0, "delegate not called for ephemeral WriteAgentLog");
  }

  // ---- persistent: WriteAgentLog delegates ----
  {
    SpyLogWriter spy;
    EphemeralSessionRegistry reg;
    // "pg-log-1" not in registry → treated as persistent

    RoutingAgentLogWriter writer(&spy, &reg);
    writer.WriteAgentLog(MakeEntry("pg-log-1"));
    failures += Check(spy.write_calls == 1, "delegate called once for persistent WriteAgentLog");
  }

  // ---- delegate=null, persistent: no crash ----
  {
    EphemeralSessionRegistry reg;
    RoutingAgentLogWriter writer(nullptr, &reg);
    writer.WriteAgentLog(MakeEntry("pg-null-log"));  // must not throw
    failures += Check(true, "no crash with null delegate for persistent");
  }

  if (failures == 0) {
    std::cout << "routing_agent_log_writer_test: all tests passed\n";
    return 0;
  }
  std::cerr << "routing_agent_log_writer_test: " << failures << " test(s) FAILED\n";
  return 1;
}

}  // namespace
