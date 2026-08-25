#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "fob/sim/v1/sim.pb.h"

namespace cex::venues::app {

// F-20 Phase 4 (T-F20-401). In-memory registry of live SimSessions.
//
// Populated later by the `sim.config` Kafka consumer and the
// SimSessionManager gRPC service (separate PRs). This class is PURE state +
// deterministic resolution — no Kafka, no threads of its own. It is
// thread-safe so the VenueSimRouter can read concurrently while the
// control-plane upserts/removes sessions.
//
// Resolution rule (Resolve): a session governs a (venue, instrument) iff
//   status == ACTIVE
//   AND (scope_venues empty      OR contains venue_id)
//   AND (scope_instruments empty OR contains symbol)
// Empty scope == wildcard. If several ACTIVE sessions match, the most
// recently activated wins (tie-break: activated_at desc, then
// sim_session_id desc) so resolution is deterministic and replay-stable.
class SimSessionRegistry {
 public:
  // Insert or replace by sim_session_id. A terminal status
  // (COMPLETED/CANCELLED) erases the entry — equivalent to Remove — so the
  // store only ever holds live (ACTIVE/PAUSED) sessions. Empty id is ignored.
  void Upsert(const fob::sim::v1::SimSession& session);

  // Remove by id (sim.config DELETE / CompleteSession). No-op if absent.
  void Remove(const std::string& sim_session_id);

  // Resolve the governing ACTIVE session for a child order, or nullopt.
  std::optional<fob::sim::v1::SimSession> Resolve(
      const std::string& venue_id, const std::string& symbol) const;

  std::size_t Size() const;         // total stored (ACTIVE + PAUSED)
  std::size_t ActiveCount() const;  // ACTIVE only

 private:
  mutable std::mutex mu_;
  std::map<std::string, fob::sim::v1::SimSession> sessions_;  // keyed by id
};

}  // namespace cex::venues::app
