#pragma once

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/replay_session.hpp"

namespace cex::backtest::infra {

// Thread-safe in-memory store for ephemeral (persist=false) ReplaySessions.
//
// Ephemeral sessions are never written to PostgreSQL or ClickHouse.  The
// registry is the sole source of truth for their lifecycle.  Once a session
// reaches a terminal state (kCompleted | kFailed | kCancelled) and the caller
// invokes EvictIfTerminal, the entry is removed; subsequent lookups return
// nullopt, making GET /sessions/{id} return 404 as documented in F15-PERSIST-2.
//
// F15-PERSIST guarantees:
//   - Insert: asserts session.persist == false.
//   - Get: returns nullopt after eviction (consistent 404).
//   - List: not exposed — ephemeral sessions are excluded from the listing
//     by design (List always delegates to the PG repo, which knows nothing
//     about them).
class EphemeralSessionRegistry {
 public:
  EphemeralSessionRegistry() = default;

  // Insert a new ephemeral session.  Precondition: session.persist == false.
  // No-op if the same session_id was already inserted (idempotent).
  void Insert(const app::ReplaySession& session);

  // Returns true if session_id is currently tracked in this registry.
  // Returns false after eviction or for ids never inserted.
  bool IsEphemeral(const std::string& session_id) const;

  // Returns the stored session, or nullopt if not present (never inserted or
  // already evicted).
  std::optional<app::ReplaySession> Get(const std::string& session_id) const;

  // Apply a state patch to an ephemeral session (mirrors PG UpdateState
  // semantics: only non-nullopt patch fields are merged).
  // Returns true on success, false if session_id is not in registry.
  bool ApplyPatch(const std::string& session_id,
                  const app::ReplaySessionStatePatch& patch);

  // Remove the session if it has reached a terminal status
  // (kCompleted | kFailed | kCancelled).  Returns true if the session was
  // present and in a terminal state (and was removed); false otherwise.
  bool EvictIfTerminal(const std::string& session_id);

  // Walk the retry_parent_id chain within the registry and return all sessions
  // in the chain (starting from the oldest ancestor that is still present).
  // Sessions evicted before this call are not included.
  std::vector<app::ReplaySession> GetRetryChain(const std::string& session_id) const;

 private:
  mutable std::shared_mutex mu_;
  std::unordered_map<std::string, app::ReplaySession> sessions_;
  // Separate set for O(1) IsEphemeral without a map lookup. An id is inserted
  // on Insert and erased on EvictIfTerminal. After eviction IsEphemeral returns
  // false, so routing falls through to the PG repo, which returns nullopt for
  // an id that was never persisted — producing correct 404 semantics.
  std::unordered_set<std::string> ephemeral_ids_;
};

}  // namespace cex::backtest::infra
