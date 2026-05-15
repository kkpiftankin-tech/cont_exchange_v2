#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cex::common {

enum class ReplayCommandType {
  kStart,
  kCancel,
  kRetry,
  kUnknown,
};

struct ReplayCommandMessage {
  std::string command_id;
  std::string session_id;
  ReplayCommandType command_type{ReplayCommandType::kUnknown};
  std::string requested_by;
  int64_t created_at_ms{0};
  std::string payload_json;
};

enum class ReplayResultKind {
  kProgress,
  kLifecycle,
  kUnknown,
};

struct ReplayResultMessage {
  ReplayResultKind kind{ReplayResultKind::kUnknown};
  std::string session_id;
  int64_t published_at_ms{0};

  uint32_t batch_seq{0};
  uint32_t total_batches{0};

  std::string status;
  bool has_summary{false};
  uint32_t processed_batches{0};
  uint32_t failed_batches{0};
  uint32_t total_fill_events{0};
  bool partial{false};
  double total_pnl{0.0};
  double avg_pnl{0.0};
  double avg_is{0.0};
  double sharpe{0.0};
  double avg_fill_rate{0.0};
  double avg_solve_time_ms{0.0};
  double max_drawdown{0.0};
  // F15-BACKTEST-8: extra summary fields kept on the wire so consumers
  // (UI / compare service) can render the full ReplaySummary without
  // round-tripping to PostgreSQL.
  double std_pnl{0.0};
  double avg_vwap{0.0};
  std::string error_details;
  std::string error_code;
};

std::string ReplayCommandTypeToString(ReplayCommandType type);
ReplayCommandType ReplayCommandTypeFromString(const std::string& type);

std::string ReplayResultKindToString(ReplayResultKind kind);
ReplayResultKind ReplayResultKindFromString(const std::string& kind);

std::string SerializeReplayCommandMessage(const ReplayCommandMessage& msg);
bool ParseReplayCommandMessage(const std::string& payload, ReplayCommandMessage* out);

std::string SerializeReplayResultMessage(const ReplayResultMessage& msg);
bool ParseReplayResultMessage(const std::string& payload, ReplayResultMessage* out);

std::string ReplayResultMessageToJson(const ReplayResultMessage& msg);

}  // namespace cex::common
