#pragma once

#include <optional>
#include <string>
#include <vector>

#include "app/replay_session.hpp"

namespace cex::backtest::app {

class ReplaySessionRepositoryPort {
 public:
  virtual ~ReplaySessionRepositoryPort() = default;

  virtual ReplaySession Create(const ReplaySession& session) = 0;
  virtual std::optional<ReplaySession> GetById(const std::string& session_id) = 0;
  virtual std::vector<ReplaySession> List(const ReplaySessionListFilter& filter) = 0;
  virtual bool UpdateState(const std::string& session_id,
                           const ReplaySessionStatePatch& patch) = 0;
  virtual std::vector<ReplaySession> GetRetryChain(const std::string& session_id) = 0;
};

}  // namespace cex::backtest::app