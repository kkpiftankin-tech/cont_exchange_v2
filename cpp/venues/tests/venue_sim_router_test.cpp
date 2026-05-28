// F-20 VenueSimRouter + SimSessionRegistry unit suite (Phase 4, T-F20-401/402).
//
// Pure routing decision over the active SimSessions — no Kafka, no threads,
// fully deterministic.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "app/sim_session_registry.hpp"
#include "app/venue_sim_router.hpp"

namespace {

using cex::venues::app::RouteDecision;
using cex::venues::app::SimSessionRegistry;
using cex::venues::app::VenueSimRouter;

namespace simv1 = fob::sim::v1;

bool Check(bool cond, const std::string& msg) {
  if (cond) return true;
  std::cerr << "[FAIL] " << msg << std::endl;
  return false;
}

simv1::SimSession MakeSession(const std::string& id, simv1::RoutingMode mode,
                              simv1::SimSessionStatus status,
                              std::vector<std::string> venues,
                              std::vector<std::string> instruments,
                              int64_t activated_sec = 0) {
  simv1::SimSession s;
  s.set_sim_session_id(id);
  s.set_routing_mode(mode);
  s.set_status(status);
  for (auto& v : venues) s.add_scope_venues(v);
  for (auto& i : instruments) s.add_scope_instruments(i);
  s.mutable_activated_at()->set_seconds(activated_sec);
  s.mutable_latency_model()->set_p50_ms(15);
  s.mutable_impact_model()->set_model_type(simv1::IMPACT_MODEL_TYPE_LINEAR);
  s.mutable_rejection_model()->set_random_rejection_rate(0.25);
  s.set_stale_lob_threshold_ms(1500);
  s.set_partial_fill_mode(simv1::PARTIAL_FILL_MODE_PROPORTIONAL);
  return s;
}

// 1. No session -> LIVE_ONLY, opt-in safety.
bool test_no_session_live_only() {
  SimSessionRegistry reg;
  VenueSimRouter router(reg);
  auto d = router.Decide("binance", "BTC/USDT");
  bool ok = true;
  ok &= Check(!d.has_session, "no session: has_session false");
  ok &= Check(d.mode == simv1::ROUTING_MODE_LIVE_ONLY, "no session: LIVE_ONLY");
  ok &= Check(!d.RoutesToSim(), "no session: not routed to sim");
  ok &= Check(d.RoutesToLive(), "no session: routed to live");
  return ok;
}

// 2. SIM_ONLY session matching venue+instrument -> SIM_ONLY + models.
bool test_sim_only_match() {
  SimSessionRegistry reg;
  reg.Upsert(MakeSession("s1", simv1::ROUTING_MODE_SIM_ONLY,
                         simv1::SIM_SESSION_STATUS_ACTIVE, {"binance"},
                         {"BTC/USDT"}));
  VenueSimRouter router(reg);
  auto d = router.Decide("binance", "BTC/USDT");
  bool ok = true;
  ok &= Check(d.has_session && d.sim_session_id == "s1", "sim_only: session s1");
  ok &= Check(d.mode == simv1::ROUTING_MODE_SIM_ONLY, "sim_only: mode");
  ok &= Check(d.RoutesToSim() && !d.RoutesToLive(), "sim_only: sim not live");
  ok &= Check(d.models.latency.p50_ms() == 15, "sim_only: latency model copied");
  ok &= Check(d.models.impact.model_type() == simv1::IMPACT_MODEL_TYPE_LINEAR,
              "sim_only: impact model copied");
  return ok;
}

// 3. Explicit LIVE_ONLY session matching -> tracked passthrough.
bool test_live_only_session() {
  SimSessionRegistry reg;
  reg.Upsert(MakeSession("s2", simv1::ROUTING_MODE_LIVE_ONLY,
                         simv1::SIM_SESSION_STATUS_ACTIVE, {"binance"},
                         {"BTC/USDT"}));
  VenueSimRouter router(reg);
  auto d = router.Decide("binance", "BTC/USDT");
  bool ok = true;
  ok &= Check(d.has_session, "live_only: has_session true");
  ok &= Check(d.mode == simv1::ROUTING_MODE_LIVE_ONLY, "live_only: mode");
  ok &= Check(!d.RoutesToSim() && d.RoutesToLive(), "live_only: live not sim");
  return ok;
}

// 4. SHADOW session -> dual.
bool test_shadow_session() {
  SimSessionRegistry reg;
  reg.Upsert(MakeSession("s3", simv1::ROUTING_MODE_SHADOW,
                         simv1::SIM_SESSION_STATUS_ACTIVE, {"binance"},
                         {"BTC/USDT"}));
  VenueSimRouter router(reg);
  auto d = router.Decide("binance", "BTC/USDT");
  bool ok = true;
  ok &= Check(d.mode == simv1::ROUTING_MODE_SHADOW, "shadow: mode");
  ok &= Check(d.RoutesToSim() && d.RoutesToLive(), "shadow: both sim+live");
  return ok;
}

// 5. Venue scope miss -> no session.
bool test_venue_scope_miss() {
  SimSessionRegistry reg;
  reg.Upsert(MakeSession("s4", simv1::ROUTING_MODE_SIM_ONLY,
                         simv1::SIM_SESSION_STATUS_ACTIVE, {"binance"},
                         {"BTC/USDT"}));
  VenueSimRouter router(reg);
  auto d = router.Decide("coinbase", "BTC/USDT");
  return Check(!d.has_session && d.mode == simv1::ROUTING_MODE_LIVE_ONLY,
               "venue scope miss -> LIVE_ONLY");
}

// 6. Wildcard scope (empty venues+instruments) governs anything.
bool test_wildcard_scope() {
  SimSessionRegistry reg;
  reg.Upsert(MakeSession("s5", simv1::ROUTING_MODE_SIM_ONLY,
                         simv1::SIM_SESSION_STATUS_ACTIVE, {}, {}));
  VenueSimRouter router(reg);
  auto a = router.Decide("kraken", "ETH/USDT");
  auto b = router.Decide("anyvenue", "ANY/PAIR");
  bool ok = true;
  ok &= Check(a.has_session && a.mode == simv1::ROUTING_MODE_SIM_ONLY,
              "wildcard: matches kraken/ETH");
  ok &= Check(b.has_session, "wildcard: matches arbitrary");
  return ok;
}

// 7. Instrument scope miss -> no session.
bool test_instrument_scope_miss() {
  SimSessionRegistry reg;
  reg.Upsert(MakeSession("s6", simv1::ROUTING_MODE_SIM_ONLY,
                         simv1::SIM_SESSION_STATUS_ACTIVE, {}, {"ETH/USDT"}));
  VenueSimRouter router(reg);
  auto d = router.Decide("binance", "BTC/USDT");
  return Check(!d.has_session, "instrument scope miss -> no session");
}

// 8. PAUSED session is ignored.
bool test_paused_ignored() {
  SimSessionRegistry reg;
  reg.Upsert(MakeSession("s7", simv1::ROUTING_MODE_SIM_ONLY,
                         simv1::SIM_SESSION_STATUS_PAUSED, {"binance"},
                         {"BTC/USDT"}));
  VenueSimRouter router(reg);
  auto d = router.Decide("binance", "BTC/USDT");
  bool ok = true;
  ok &= Check(!d.has_session, "paused: not resolved");
  ok &= Check(reg.Size() == 1 && reg.ActiveCount() == 0,
              "paused: stored but not active");
  return ok;
}

// 9. Upsert with terminal status removes the entry.
bool test_completed_upsert_removes() {
  SimSessionRegistry reg;
  reg.Upsert(MakeSession("s8", simv1::ROUTING_MODE_SIM_ONLY,
                         simv1::SIM_SESSION_STATUS_ACTIVE, {"binance"},
                         {"BTC/USDT"}));
  bool ok = Check(reg.ActiveCount() == 1, "completed: active before");
  reg.Upsert(MakeSession("s8", simv1::ROUTING_MODE_SIM_ONLY,
                         simv1::SIM_SESSION_STATUS_COMPLETED, {"binance"},
                         {"BTC/USDT"}));
  VenueSimRouter router(reg);
  auto d = router.Decide("binance", "BTC/USDT");
  ok &= Check(!d.has_session, "completed: not resolved after");
  ok &= Check(reg.Size() == 0, "completed: erased");
  return ok;
}

// 10. Remove() drops a session.
bool test_remove() {
  SimSessionRegistry reg;
  reg.Upsert(MakeSession("s9", simv1::ROUTING_MODE_SIM_ONLY,
                         simv1::SIM_SESSION_STATUS_ACTIVE, {}, {}));
  reg.Remove("s9");
  VenueSimRouter router(reg);
  return Check(!router.Decide("x", "y").has_session && reg.Size() == 0,
               "remove: gone");
}

// 11. Two ACTIVE matches -> most recently activated wins, deterministically.
bool test_tie_break_latest_activated() {
  SimSessionRegistry reg;
  reg.Upsert(MakeSession("old", simv1::ROUTING_MODE_SIM_ONLY,
                         simv1::SIM_SESSION_STATUS_ACTIVE, {}, {}, 100));
  reg.Upsert(MakeSession("new", simv1::ROUTING_MODE_SHADOW,
                         simv1::SIM_SESSION_STATUS_ACTIVE, {}, {}, 200));
  VenueSimRouter router(reg);
  auto d = router.Decide("binance", "BTC/USDT");
  bool ok = true;
  ok &= Check(d.sim_session_id == "new", "tie-break: latest activated wins");
  ok &= Check(d.mode == simv1::ROUTING_MODE_SHADOW, "tie-break: newer mode");
  return ok;
}

// 12. Session knobs (stale threshold, partial-fill, rejection) propagate.
bool test_model_propagation() {
  SimSessionRegistry reg;
  reg.Upsert(MakeSession("s10", simv1::ROUTING_MODE_SIM_ONLY,
                         simv1::SIM_SESSION_STATUS_ACTIVE, {"binance"},
                         {"BTC/USDT"}));
  VenueSimRouter router(reg);
  auto d = router.Decide("binance", "BTC/USDT");
  bool ok = true;
  ok &= Check(d.stale_lob_threshold_ms == 1500, "propagation: stale threshold");
  ok &= Check(d.partial_fill_mode == simv1::PARTIAL_FILL_MODE_PROPORTIONAL,
              "propagation: partial fill mode");
  ok &= Check(d.models.rejection.random_rejection_rate() == 0.25,
              "propagation: rejection rate");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) { std::cerr << "  in test: " << name << std::endl; ok = false; }
  };

  run("no_session_live_only", test_no_session_live_only);
  run("sim_only_match", test_sim_only_match);
  run("live_only_session", test_live_only_session);
  run("shadow_session", test_shadow_session);
  run("venue_scope_miss", test_venue_scope_miss);
  run("wildcard_scope", test_wildcard_scope);
  run("instrument_scope_miss", test_instrument_scope_miss);
  run("paused_ignored", test_paused_ignored);
  run("completed_upsert_removes", test_completed_upsert_removes);
  run("remove", test_remove);
  run("tie_break_latest_activated", test_tie_break_latest_activated);
  run("model_propagation", test_model_propagation);

  if (ok) {
    std::cout << "[OK] venue_sim_router_test passed (12 cases: routing modes, "
                 "scope match/miss, wildcard, paused/terminal, remove, "
                 "deterministic tie-break, model propagation)."
              << std::endl;
    return 0;
  }
  return 1;
}
