#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

#include "app/replay_orchestration_ports.hpp"
#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"

namespace cex::backtest::app {

// Handles CancelReplayCommand from replay.commands Kafka topic.
// Implements ICancellationToken so it can be passed directly to RunReplaySession.
//
// For pending sessions: transitions to kCancelled in DB immediately and sets a
// session-scoped token so a concurrently preparing orchestrator cannot promote
// the same session to running.
// For running sessions: sets the cancellation flag; RunReplaySession finishes
// the current batch, saves partial summary/logs, and persists kCancelled.
class CancelReplaySession : public ICancellationToken {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;

  struct Dependencies {
    ReplaySessionRepositoryPort* session_repo{nullptr};
    IReplaySummaryStore* summary_store{nullptr};
    IReplayEventPublisher* event_publisher{nullptr};
  };

  struct Request {
    std::string session_id;
    std::optional<std::string> requested_by;
    std::optional<std::string> reason;
  };

  struct Result {
    bool ok{false};
    ReplaySession session;
    bool cancellation_dispatched{false};  // true when running session's token was set
    std::string error_code;
    std::string error_message;
  };

  explicit CancelReplaySession(Dependencies deps, Clock clock = nullptr);

  // ICancellationToken: polled by RunReplaySession before each batch.
  bool IsCancelled() const override;
  bool IsCancelled(const std::string& session_id) const override;

  Result Run(const Request& request);

 private:
  std::chrono::system_clock::time_point Now() const;

  Dependencies deps_;
  Clock clock_;
  mutable std::mutex cancelled_mu_;
  std::unordered_set<std::string> cancelled_session_ids_;
};

}  // namespace cex::backtest::app
