#include "app/sim_session_registry.hpp"

namespace cex::venues::app {
namespace {

// Empty scope list == wildcard (governs all). Otherwise membership test.
template <typename Scope>
bool InScope(const Scope& scope, const std::string& value) {
  if (scope.empty()) return true;
  for (const auto& s : scope) {
    if (s == value) return true;
  }
  return false;
}

bool IsTerminal(fob::sim::v1::SimSessionStatus status) {
  return status == fob::sim::v1::SIM_SESSION_STATUS_COMPLETED ||
         status == fob::sim::v1::SIM_SESSION_STATUS_CANCELLED;
}

// Returns true if `a` is preferred over `b`: activated_at desc, then
// sim_session_id desc. Deterministic — never returns true for equal sessions.
bool MoreRecent(const fob::sim::v1::SimSession& a,
                const fob::sim::v1::SimSession& b) {
  const auto& ta = a.activated_at();
  const auto& tb = b.activated_at();
  if (ta.seconds() != tb.seconds()) return ta.seconds() > tb.seconds();
  if (ta.nanos() != tb.nanos()) return ta.nanos() > tb.nanos();
  return a.sim_session_id() > b.sim_session_id();
}

}  // namespace

void SimSessionRegistry::Upsert(const fob::sim::v1::SimSession& session) {
  std::lock_guard<std::mutex> lock(mu_);
  if (session.sim_session_id().empty()) return;
  if (IsTerminal(session.status())) {
    sessions_.erase(session.sim_session_id());
    return;
  }
  sessions_[session.sim_session_id()] = session;
}

void SimSessionRegistry::Remove(const std::string& sim_session_id) {
  std::lock_guard<std::mutex> lock(mu_);
  sessions_.erase(sim_session_id);
}

std::optional<fob::sim::v1::SimSession> SimSessionRegistry::Resolve(
    const std::string& venue_id, const std::string& symbol) const {
  std::lock_guard<std::mutex> lock(mu_);
  const fob::sim::v1::SimSession* best = nullptr;
  for (const auto& [id, s] : sessions_) {
    if (s.status() != fob::sim::v1::SIM_SESSION_STATUS_ACTIVE) continue;
    if (!InScope(s.scope_venues(), venue_id)) continue;
    if (!InScope(s.scope_instruments(), symbol)) continue;
    if (best == nullptr || MoreRecent(s, *best)) best = &s;
  }
  if (best == nullptr) return std::nullopt;
  return *best;
}

std::size_t SimSessionRegistry::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sessions_.size();
}

std::size_t SimSessionRegistry::ActiveCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::size_t n = 0;
  for (const auto& [id, s] : sessions_) {
    if (s.status() == fob::sim::v1::SIM_SESSION_STATUS_ACTIVE) ++n;
  }
  return n;
}

}  // namespace cex::venues::app
