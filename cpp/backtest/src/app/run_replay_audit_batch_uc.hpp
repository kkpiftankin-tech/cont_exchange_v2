#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app/historical_batch_loader_port.hpp"
#include "app/historical_batch_loader_uc.hpp"
#include "app/replay_config_snapshot.hpp"
#include "app/replay_orchestration_ports.hpp"
#include "app/shadow_namespace_uc.hpp"

namespace cex::backtest::app {

struct AuditBatchSnapshot {
  std::string batch_id;
  std::string clear_prices_json;
  std::string executed_rates_json;
  std::vector<HistoricalFillRow> fills;
  double residual_norm{0.0};
  uint32_t solve_time_ms{0};
  std::string risk_status{"ok"};
};

struct AuditStringDiff {
  bool equivalent{false};
  std::string production_value;
  std::string replay_value;
};

struct AuditDoubleDiff {
  bool equivalent{false};
  double production_value{0.0};
  double replay_value{0.0};
  double abs_diff{0.0};
};

struct AuditUInt32Diff {
  bool equivalent{false};
  uint32_t production_value{0};
  uint32_t replay_value{0};
  uint32_t abs_diff{0};
};

struct ReplayAuditDiff {
  AuditStringDiff clear_prices;
  AuditStringDiff executed_rates;
  AuditStringDiff fills;
  AuditDoubleDiff residual_norm;
  AuditUInt32Diff solve_time_ms;
  AuditStringDiff risk_status;
  bool equivalent{false};
};

struct AuditExecutionResult {
  bool ok{false};
  AuditBatchSnapshot snapshot;
  std::string error_code;
  std::string error_details;
  FailureComponent failure_component{FailureComponent::kUnknown};
};

class IAuditBatchExecutor {
 public:
  virtual ~IAuditBatchExecutor() = default;
  virtual AuditExecutionResult ExecuteAuditBatch(
      const std::string& namespace_id,
      const std::string& session_config_snapshot_json,
      const HistoricalBatch& batch) = 0;
};

class RunReplayAuditBatch {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;

  struct Dependencies {
    ReplayConfigSnapshotBuilder* config_builder{nullptr};
    ShadowNamespaceInitializer* shadow_init{nullptr};
    IAuditHistoricalLoader* audit_loader{nullptr};
    IAuditBatchExecutor* audit_executor{nullptr};
  };

  struct Request {
    std::string audit_run_id;
    std::string batch_id;
    std::string tracked_user_id;
    std::string reporting_currency{"USDT"};
    ReplayConfigRequest config_request;
  };

  struct Result {
    bool ok{false};
    bool equivalent{false};
    std::string error_code;
    std::string error_details;
    SessionConfigSnapshot config_snapshot;
    AuditBatchSnapshot production_snapshot;
    AuditBatchSnapshot replay_snapshot;
    ReplayAuditDiff diff;
  };

  RunReplayAuditBatch(Dependencies deps, Clock clock = nullptr);

  Result Run(const Request& request) const;

 private:
  std::chrono::system_clock::time_point Now() const;

  Dependencies deps_;
  Clock clock_;
};

}  // namespace cex::backtest::app
