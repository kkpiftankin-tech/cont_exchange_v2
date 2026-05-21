#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app/historical_batch_loader_port.hpp"
#include "app/replay_config_snapshot.hpp"
#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"
#include "app/rbac_engine.hpp"

namespace cex::backtest::app {

// Validates replay session creation inputs, checks historical availability,
// materializes deterministic config snapshot and creates a replay session in
// "pending" state.

class CreateReplaySession {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;
  using IdGenerator = std::function<std::string()>;

  struct Dependencies {
    ReplaySessionRepositoryPort* session_repo{nullptr};
    ReplayConfigSnapshotBuilder* config_builder{nullptr};
    IHistoricalBatchLoader* historical_loader{nullptr};
    RbacEngine* rbac_engine{nullptr};
  };

  struct FlowOrderParams {
    std::string symbol;
    std::string side;
    double pL{0.0};
    double pH{0.0};
    double qrate{0.0};
    double qmax{0.0};
    int64_t executionwindow{0};
    bool has_executionwindow{true};
  };

  struct Request {
    std::optional<std::string> session_id;
    std::string user_id;
    std::string name;
    std::chrono::system_clock::time_point date_range_from;
    std::chrono::system_clock::time_point date_range_to;

    // Request supports either typed strategy entries or raw strategy JSON.
    std::vector<FlowOrderParams> strategy;
    std::optional<std::string> strategy_json;
    std::optional<std::string> instrument;

    std::string solver_config_id;
    std::string risk_limits_id;
    std::optional<std::string> solver_config_inline_json;
    std::optional<std::string> risk_limits_inline_json;

    std::optional<std::string> fee_model_id;
    std::optional<std::string> fee_model_inline_json;

    std::optional<std::string> reward_mode;
    std::optional<std::string> reward_config_id;
    std::optional<std::string> reward_config_inline_json;

    std::optional<int64_t> random_seed;
    std::optional<double> tolerance;
  };

  struct Result {
    bool ok{false};
    ReplaySession created;
    std::string error_code;
    std::string error_message;
  };

  CreateReplaySession(Dependencies deps,
                      Clock clock = nullptr,
                      IdGenerator id_generator = nullptr);

  Result Run(const Request& request) const;

 private:
  std::chrono::system_clock::time_point Now() const;
  std::string GenerateId() const;

  Dependencies deps_;
  Clock clock_;
  IdGenerator id_generator_;
};

}  // namespace cex::backtest::app
