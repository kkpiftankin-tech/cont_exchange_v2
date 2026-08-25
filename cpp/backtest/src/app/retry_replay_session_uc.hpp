#pragma once

#include <optional>
#include <string>

#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"

namespace cex::backtest::app {

struct RetryReplaySessionRequest {
  std::string original_session_id;
  std::string user_id;
  std::optional<std::string> new_session_id;
};

struct RetryReplaySessionResponse {
  bool ok{false};
  std::string new_session_id;
  std::optional<ReplaySession> created_session;
  std::string error_code;
  std::string error_message;
};

class RetryReplaySessionUseCase {
 public:
  struct Dependencies {
    ReplaySessionRepositoryPort* session_repo{nullptr};
    // опционально: RunReplaySessionUseCase* run_replay_uc{nullptr};
  };

  explicit RetryReplaySessionUseCase(Dependencies deps);

  RetryReplaySessionResponse Execute(const RetryReplaySessionRequest& request);

 private:
  Dependencies deps_;
  
  std::string GenerateNewSessionId() const;
  ReplaySession CreateRetrySessionFromOriginal(
      const ReplaySession& original,
      const std::optional<std::string>& requested_session_id) const;
};

}  // namespace cex::backtest::app
