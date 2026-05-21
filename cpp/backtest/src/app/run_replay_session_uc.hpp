#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "app/historical_batch_loader_uc.hpp"
#include "app/replay_config_snapshot.hpp"
#include "app/replay_orchestration_ports.hpp"
#include "app/replay_runtime_metrics.hpp"
#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"
#include "app/restore_state_uc.hpp"
#include "app/shadow_namespace_uc.hpp"
#include "app/rbac_engine.hpp"

namespace cex::backtest::app {

// F15-BACKTEST-4. End-to-end orchestrator for one replay session:
//   pending -> running -> (completed | failed | cancelled).
//
// Glue points:
//   * F15-BACKTEST-1 historical loader for batches/fills/marketdata
//   * F15-BACKTEST-2 config snapshot builder
//   * F15-BACKTEST-3 shadow namespace initializer
//   * F15-BACKTEST-6 IAgentLogWriter (per-batch logs)
//   * F15-BACKTEST-7 IReplaySummaryStore (final summary)
//   * F15-BACKTEST-8 IReplayEventPublisher (progress / lifecycle)
//   * F15-CORE-* IBatchExecutor (matching/risk/ledger pipeline)
class RunReplaySession {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;

  struct Dependencies {
    ReplaySessionRepositoryPort* session_repo{nullptr};
    ReplayConfigSnapshotBuilder* config_builder{nullptr};
    ShadowNamespaceInitializer* shadow_init{nullptr};
    IShadowLedger* shadow_ledger{nullptr};
    HistoricalBatchLoaderUseCases* history_loader{nullptr};
    IBatchExecutor* batch_executor{nullptr};
    IAgentLogWriter* agent_log_writer{nullptr};
    IReplaySummaryStore* summary_store{nullptr};
    IReplayEventPublisher* event_publisher{nullptr};
    ReplayRuntimeMetrics* runtime_metrics{nullptr};
    ICancellationToken* cancellation_token{nullptr};
    RbacEngine* rbac_engine{nullptr};
    // F15-BACKTEST-5: optional RestoreState use case. When supplied, replay
    // restart for a duplicate batch_id is delegated to RestoreState instead of
    // calling the shadow ledger directly. Optional so existing wiring keeps
    // working unchanged.
    RestoreState* restore_state{nullptr};
    IAgentLogReader* agent_log_reader{nullptr};
  };

  struct Request {
    std::string session_id;
    std::string tracked_user_id;
    std::string reporting_currency{"USDT"};
    ReplayConfigRequest config_request;
    HistoricalBatchLoaderUseCases::Config history_config;
  };

  struct Result {
    ReplaySessionStatus final_status{ReplaySessionStatus::kFailed};
    ReplaySummary summary;
    std::string error_details;
    // F15-BACKTEST-8: machine-readable error code surfaced to the lifecycle
    // event payload alongside the human-readable error_details.
    std::string error_code;
  };

  RunReplaySession(Dependencies deps, Clock clock = nullptr);

  Result Run(const Request& request);

 private:
  Result FailFromInit(const std::string& session_id,
                      FailureComponent component,
                      const std::string& error_code,
                      const std::string& error);
  std::chrono::system_clock::time_point Now() const;

  Dependencies deps_;
  Clock clock_;
  NeverCancelledToken default_token_;
};

}  // namespace cex::backtest::app