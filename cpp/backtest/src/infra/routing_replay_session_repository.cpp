#include "infra/routing_replay_session_repository.hpp"

#include <stdexcept>

namespace cex::backtest::infra {

RoutingReplaySessionRepository::RoutingReplaySessionRepository(
    app::ReplaySessionRepositoryPort* pg_repo,
    EphemeralSessionRegistry* registry)
    : pg_repo_(pg_repo), registry_(registry) {}

app::ReplaySession RoutingReplaySessionRepository::Create(
    const app::ReplaySession& session) {
  if (!session.persist) {
    // Ephemeral path: insert into in-memory registry only.
    registry_->Insert(session);
    return session;
  }
  // Persistent path: requires PostgreSQL.
  if (pg_repo_ == nullptr) {
    throw std::runtime_error(
        "persist=true requires PostgreSQL but BACKTEST_POSTGRES_DSN is not configured");
  }
  return pg_repo_->Create(session);
}

std::optional<app::ReplaySession> RoutingReplaySessionRepository::GetById(
    const std::string& session_id) {
  if (registry_->IsEphemeral(session_id)) {
    return registry_->Get(session_id);
  }
  if (pg_repo_ == nullptr) return std::nullopt;
  return pg_repo_->GetById(session_id);
}

std::vector<app::ReplaySession> RoutingReplaySessionRepository::List(
    const app::ReplaySessionListFilter& filter) {
  // Ephemeral sessions are intentionally excluded from list (F15-PERSIST-3).
  if (pg_repo_ == nullptr) return {};
  return pg_repo_->List(filter);
}

bool RoutingReplaySessionRepository::UpdateState(
    const std::string& session_id,
    const app::ReplaySessionStatePatch& patch) {
  if (registry_->IsEphemeral(session_id)) {
    return registry_->ApplyPatch(session_id, patch);
  }
  if (pg_repo_ == nullptr) return false;
  return pg_repo_->UpdateState(session_id, patch);
}

std::vector<app::ReplaySession> RoutingReplaySessionRepository::GetRetryChain(
    const std::string& session_id) {
  if (registry_->IsEphemeral(session_id)) {
    return registry_->GetRetryChain(session_id);
  }
  if (pg_repo_ == nullptr) return {};
  return pg_repo_->GetRetryChain(session_id);
}

}  // namespace cex::backtest::infra
