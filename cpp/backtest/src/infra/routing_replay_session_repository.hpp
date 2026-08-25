#pragma once

#include <optional>
#include <string>
#include <stdexcept>
#include <vector>

#include "app/replay_session_repository_port.hpp"
#include "infra/ephemeral_session_registry.hpp"

namespace cex::backtest::infra {

// Implements ReplaySessionRepositoryPort with transparent routing:
//
//   session.persist == false  →  delegate to EphemeralSessionRegistry
//   session.persist == true   →  delegate to the wrapped PG repository
//
// This keeps all use-case logic unchanged; the routing is invisible to
// CreateReplaySession / CancelReplaySession / RunReplaySession / RetryReplaySession.
//
// Invariants:
//   - List always delegates to pg_repo (empty vec if null) — ephemeral sessions
//     are intentionally excluded from the listing (F15-PERSIST-3).
//   - If pg_repo is null and persist==true, Create() throws std::runtime_error.
class RoutingReplaySessionRepository final
    : public app::ReplaySessionRepositoryPort {
 public:
  // pg_repo may be nullptr when BACKTEST_POSTGRES_DSN is not configured.
  // registry must not be nullptr.
  explicit RoutingReplaySessionRepository(
      app::ReplaySessionRepositoryPort* pg_repo,
      EphemeralSessionRegistry* registry);

  app::ReplaySession Create(const app::ReplaySession& session) override;
  std::optional<app::ReplaySession> GetById(const std::string& session_id) override;
  std::vector<app::ReplaySession> List(const app::ReplaySessionListFilter& filter) override;
  bool UpdateState(const std::string& session_id,
                   const app::ReplaySessionStatePatch& patch) override;
  std::vector<app::ReplaySession> GetRetryChain(const std::string& session_id) override;

 private:
  app::ReplaySessionRepositoryPort* pg_repo_;
  EphemeralSessionRegistry* registry_;
};

}  // namespace cex::backtest::infra
