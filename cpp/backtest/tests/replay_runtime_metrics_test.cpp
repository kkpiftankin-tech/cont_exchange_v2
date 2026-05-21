#include <cstdlib>
#include <iostream>
#include <string>

#include "app/replay_orchestration_ports.hpp"
#include "app/replay_runtime_metrics.hpp"

namespace {

using cex::backtest::app::BatchOutcome;
using cex::backtest::app::FailureComponent;
using cex::backtest::app::ReplayRuntimeMetrics;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool test_render_prometheus() {
  ReplayRuntimeMetrics metrics;
  metrics.ObserveHistoryLoad(120.0, 4, 11, 8);
  metrics.ObserveShadowNamespaceInit(6.0, true, false);
  metrics.SetShadowNamespacesActive(2);
  metrics.ObserveBatchExecution(
      25.0, 9, 300, BatchOutcome::kOk, FailureComponent::kUnknown, "");
  metrics.ObserveBatchExecution(
      40.0, 13, 800, BatchOutcome::kSoftFailure, FailureComponent::kSolver, "solver diverged");
  metrics.ObserveBatchExecution(
      55.0, 17, 1600, BatchOutcome::kHardFailure, FailureComponent::kLedger, "ledger broke");
  metrics.ObserveClickHouseInsert("backtest.replay_agentlogs", 18.0, true);
  metrics.ObserveClickHouseInsert("backtest.replay_agentlogs", 44.0, false);
  metrics.ObserveKafkaPublish("replay.results", 3.0, true);
  metrics.ObserveKafkaPublish("replay.results", 7.0, false);

  const std::string rendered = metrics.RenderPrometheus();
  if (!Check(Contains(rendered, "backtest_replay_batches_loaded_total 4"),
             "batches counter rendered")) return false;
  if (!Check(Contains(rendered, "backtest_replay_failures_total{failure_type=\"soft\",component=\"solver\"} 1"),
             "soft failure classified")) return false;
  if (!Check(Contains(rendered, "backtest_replay_failures_total{failure_type=\"hard\",component=\"ledger\"} 1"),
             "hard failure classified")) return false;
  if (!Check(Contains(rendered, "backtest_replay_shadow_namespaces_active 2"),
             "shadow active gauge rendered")) return false;
  if (!Check(Contains(rendered, "backtest_replay_clickhouse_insert_total{table=\"backtest.replay_agentlogs\",result=\"error\"} 1"),
             "clickhouse error rendered")) return false;
  if (!Check(Contains(rendered, "backtest_replay_kafka_publish_total{topic=\"replay.results\",result=\"error\"} 1"),
             "kafka error rendered")) return false;
  if (!Check(Contains(rendered, "backtest_replay_progress_lag_ms_bucket"),
             "progress lag histogram rendered")) return false;
  return true;
}

}  // namespace

int main() {
  if (test_render_prometheus()) {
    std::cout << "[OK] backtest_replay_runtime_metrics_test passed" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
