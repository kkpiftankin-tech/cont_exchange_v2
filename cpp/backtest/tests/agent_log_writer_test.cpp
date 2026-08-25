#include <iostream>
#include <limits>
#include <string>

#include "app/replay_orchestration_ports.hpp"
#include "infra/clickhouse_storage.hpp"

namespace {

using cex::backtest::app::AgentLogEntry;
using cex::backtest::app::FailureComponent;
using cex::backtest::infra::BuildAgentLogJsonRow;

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

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

AgentLogEntry MakeFullEntry() {
  AgentLogEntry e;
  e.log_id = "sess-1:7";
  e.session_id = "sess-1";
  e.original_batch_id = "batch-42";
  e.batch_seq = 7;
  e.event_time_ms = 1700000000000LL;
  e.pnl = 12.5;
  e.is_value = -3.25;
  e.fill_rate = 0.95;
  e.fills_applied = 4;
  e.solve_time_ms = 17;
  e.residual_norm = 1.5e-6;
  e.reward = 12.5;
  e.solver_error_flag = false;
  e.risk_status = "ok";
  e.error_code = "";
  e.error_details = "";
  e.failure_component = FailureComponent::kUnknown;
  e.state_json = "{\"position\":\"0.5\"}";
  e.action_json = "{\"q\":1}";
  e.fills_json = "[{\"id\":\"f1\"}]";
  e.batch_result_json = "{\"clear\":1}";
  e.metrics_json = "{\"sharpe\":0.1}";
  return e;
}

void TestRowContainsAllSpecFields() {
  std::cerr << "-- TestRowContainsAllSpecFields\n";
  const auto e = MakeFullEntry();
  const std::string row = BuildAgentLogJsonRow(e);
  // Spec-required fields per F15-BACKTEST-6.
  EXPECT(Contains(row, "\"state_json\":"));
  EXPECT(Contains(row, "\"action_json\":"));
  EXPECT(Contains(row, "\"reward\":12.5"));
  EXPECT(Contains(row, "\"pnl\":12.5"));
  EXPECT(Contains(row, "\"is_value\":-3.25"));
  EXPECT(Contains(row, "\"fill_rate\":0.95"));
  EXPECT(Contains(row, "\"fills_json\":"));
  EXPECT(Contains(row, "\"batch_result_json\":"));
  EXPECT(Contains(row, "\"solve_time_ms\":17"));
  EXPECT(Contains(row, "\"residual_norm\":"));
  EXPECT(Contains(row, "\"risk_status\":\"ok\""));
  EXPECT(Contains(row, "\"solver_error_flag\":0"));
  EXPECT(Contains(row, "\"original_batch_id\":\"batch-42\""));
  EXPECT(Contains(row, "\"session_id\":\"sess-1\""));
  EXPECT(Contains(row, "\"log_id\":\"sess-1:7\""));
  EXPECT(Contains(row, "\"batch_seq\":7"));
  EXPECT(Contains(row, "\"event_time_ms\":1700000000000"));
  EXPECT(Contains(row, "\"session_config_snapshot_version\":1"));
}

void TestEmptyJsonPayloadsNormalised() {
  std::cerr << "-- TestEmptyJsonPayloadsNormalised\n";
  AgentLogEntry e;
  e.session_id = "s";
  e.original_batch_id = "b";
  e.risk_status = "ok";
  // All json payloads left empty.
  const std::string row = BuildAgentLogJsonRow(e);
  // Empty objects -> {} ; the JSON-escaped form embeds {} inside string quotes.
  EXPECT(Contains(row, "\"state_json\":\"{}\""));
  EXPECT(Contains(row, "\"action_json\":\"{}\""));
  EXPECT(Contains(row, "\"batch_result_json\":\"{}\""));
  EXPECT(Contains(row, "\"metrics_json\":\"{}\""));
  EXPECT(Contains(row, "\"fills_json\":\"[]\""));
}

void TestStringEscaping() {
  std::cerr << "-- TestStringEscaping\n";
  AgentLogEntry e;
  e.session_id = "s\"id";  // contains a quote
  e.original_batch_id = "b\\1";  // contains a backslash
  e.error_details = "line1\nline2";
  e.risk_status = "soft";
  const std::string row = BuildAgentLogJsonRow(e);
  EXPECT(Contains(row, "\"session_id\":\"s\\\"id\""));
  EXPECT(Contains(row, "\"original_batch_id\":\"b\\\\1\""));
  EXPECT(Contains(row, "\"error_details\":\"line1\\nline2\""));
}

void TestRiskStatusFallbackOnSolverError() {
  std::cerr << "-- TestRiskStatusFallbackOnSolverError\n";
  AgentLogEntry e;
  e.session_id = "s";
  e.original_batch_id = "b";
  e.solver_error_flag = true;
  e.risk_status = "";  // empty -> writer should derive "hard"
  const std::string row = BuildAgentLogJsonRow(e);
  EXPECT(Contains(row, "\"risk_status\":\"hard\""));
  EXPECT(Contains(row, "\"solver_error_flag\":1"));
}

void TestFailureComponentSerialised() {
  std::cerr << "-- TestFailureComponentSerialised\n";
  AgentLogEntry e;
  e.session_id = "s";
  e.original_batch_id = "b";
  e.failure_component = FailureComponent::kSolver;
  e.risk_status = "hard";
  const std::string row = BuildAgentLogJsonRow(e);
  EXPECT(Contains(row, "\"failure_component\":\"solver\""));
}

void TestRowIsStableAcrossCalls() {
  std::cerr << "-- TestRowIsStableAcrossCalls (idempotency precondition)\n";
  // ReplacingMergeTree dedup hinges on a stable PK + version. The PK columns
  // (session_id, original_batch_id) must serialise identically for two calls
  // with identical inputs so re-runs collide on the same row.
  const auto e = MakeFullEntry();
  const std::string a = BuildAgentLogJsonRow(e);
  const std::string b = BuildAgentLogJsonRow(e);
  EXPECT(a == b);
}

void TestBoundaryNumericValuesAreSerialized() {
  std::cerr << "-- TestBoundaryNumericValuesAreSerialized\n";
  AgentLogEntry e;
  e.session_id = "s";
  e.original_batch_id = "b";
  e.batch_seq = std::numeric_limits<uint32_t>::max();
  e.event_time_ms = std::numeric_limits<int64_t>::max();
  e.fills_applied = std::numeric_limits<uint32_t>::max();
  e.solve_time_ms = std::numeric_limits<uint32_t>::max();
  e.risk_status = "ok";
  const std::string row = BuildAgentLogJsonRow(e);
  EXPECT(Contains(row, "\"batch_seq\":4294967295"));
  EXPECT(Contains(row, "\"event_time_ms\":9223372036854775807"));
  EXPECT(Contains(row, "\"fills_applied\":4294967295"));
  EXPECT(Contains(row, "\"solve_time_ms\":4294967295"));
}

}  // namespace

int main() {
  TestRowContainsAllSpecFields();
  TestEmptyJsonPayloadsNormalised();
  TestStringEscaping();
  TestRiskStatusFallbackOnSolverError();
  TestFailureComponentSerialised();
  TestRowIsStableAcrossCalls();
  TestBoundaryNumericValuesAreSerialized();

  std::cerr << "\nPassed: " << g_pass << ", Failed: " << g_fail << std::endl;
  return g_fail == 0 ? 0 : 1;
}
