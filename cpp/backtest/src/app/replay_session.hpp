#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace cex::backtest::app {

enum class ReplaySessionStatus {
  kPending,
  kRunning,
  kCompleted,
  kFailed,
  kCancelled,
};

struct ReplaySession {
  std::string session_id;
  std::string user_id;
  std::string name;
  std::string strategy_json;
  std::chrono::system_clock::time_point date_range_from;
  std::chrono::system_clock::time_point date_range_to;
  std::string solver_config_id;
  std::string risk_limits_id;
  std::string fee_model_json;
  std::optional<std::string> session_config_snapshot_json;
  ReplaySessionStatus status{ReplaySessionStatus::kPending};
  std::optional<std::int32_t> total_batches;
  std::int32_t progress_batches{0};
  std::chrono::system_clock::time_point created_at;
  std::optional<std::chrono::system_clock::time_point> started_at;
  std::optional<std::chrono::system_clock::time_point> completed_at;
  std::optional<std::string> error_details;
  std::optional<std::string> retry_parent_id;
};

struct ReplaySessionListFilter {
  std::optional<std::string> user_id;
  std::optional<ReplaySessionStatus> status;
  std::optional<std::chrono::system_clock::time_point> created_from;
  std::optional<std::chrono::system_clock::time_point> created_to;
  std::optional<std::chrono::system_clock::time_point> replay_from;
  std::optional<std::chrono::system_clock::time_point> replay_to;
  std::optional<std::int32_t> limit;
  std::optional<std::int32_t> offset;
};

struct ReplaySessionStatePatch {
  std::optional<ReplaySessionStatus> status;
  std::optional<std::int32_t> total_batches;
  std::optional<std::int32_t> progress_batches;
  std::optional<std::chrono::system_clock::time_point> started_at;
  std::optional<std::chrono::system_clock::time_point> completed_at;
  std::optional<std::string> error_details;
};

}  // namespace cex::backtest::app
