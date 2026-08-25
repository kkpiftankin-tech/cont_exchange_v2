#include "infra/ephemeral_session_registry.hpp"

#include <cassert>
#include <mutex>  // std::unique_lock (not transitively provided by <shared_mutex> on libstdc++)

namespace cex::backtest::infra {

void EphemeralSessionRegistry::Insert(const app::ReplaySession& session) {
  assert(!session.persist);  // Caller must not insert persistent sessions here.
  std::unique_lock lock(mu_);
  if (ephemeral_ids_.count(session.session_id) > 0) return;  // idempotent
  sessions_[session.session_id] = session;
  ephemeral_ids_.insert(session.session_id);
}

bool EphemeralSessionRegistry::IsEphemeral(const std::string& session_id) const {
  std::shared_lock lock(mu_);
  return ephemeral_ids_.count(session_id) > 0;
}

std::optional<app::ReplaySession> EphemeralSessionRegistry::Get(
    const std::string& session_id) const {
  std::shared_lock lock(mu_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) return std::nullopt;
  return it->second;
}

bool EphemeralSessionRegistry::ApplyPatch(
    const std::string& session_id,
    const app::ReplaySessionStatePatch& patch) {
  std::unique_lock lock(mu_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) return false;

  auto& s = it->second;
  if (patch.status.has_value()) s.status = *patch.status;
  if (patch.total_batches.has_value()) s.total_batches = *patch.total_batches;
  if (patch.progress_batches.has_value()) s.progress_batches = *patch.progress_batches;
  if (patch.started_at.has_value()) s.started_at = *patch.started_at;
  if (patch.completed_at.has_value()) s.completed_at = *patch.completed_at;
  if (patch.error_details.has_value()) s.error_details = *patch.error_details;
  return true;
}

bool EphemeralSessionRegistry::EvictIfTerminal(const std::string& session_id) {
  std::unique_lock lock(mu_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) return false;

  const auto status = it->second.status;
  const bool terminal = (status == app::ReplaySessionStatus::kCompleted ||
                         status == app::ReplaySessionStatus::kFailed ||
                         status == app::ReplaySessionStatus::kCancelled);
  if (!terminal) return false;

  sessions_.erase(it);
  ephemeral_ids_.erase(session_id);
  return true;
}

std::vector<app::ReplaySession> EphemeralSessionRegistry::GetRetryChain(
    const std::string& session_id) const {
  std::shared_lock lock(mu_);

  // Walk up the retry_parent_id chain to find the root. A visited set guards
  // against a corrupt cycle (A.parent=B, B.parent=A) causing an infinite loop.
  std::vector<std::string> chain_ids;
  std::unordered_set<std::string> visited;
  std::string current = session_id;
  while (!current.empty()) {
    if (!visited.insert(current).second) break;  // cycle guard
    auto it = sessions_.find(current);
    if (it == sessions_.end()) break;
    chain_ids.push_back(current);
    if (!it->second.retry_parent_id.has_value()) break;
    current = *it->second.retry_parent_id;
  }

  // Build result in chronological order (root → leaf).
  std::vector<app::ReplaySession> result;
  result.reserve(chain_ids.size());
  for (auto rit = chain_ids.rbegin(); rit != chain_ids.rend(); ++rit) {
    auto it = sessions_.find(*rit);
    if (it != sessions_.end()) result.push_back(it->second);
  }
  return result;
}

}  // namespace cex::backtest::infra
