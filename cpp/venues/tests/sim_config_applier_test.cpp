// F-20 sim.config applier unit suite (Phase 4, T-F20-401 hot-reload core).
//
// Pure SimConfigEvent -> SimSessionRegistry mapping. No Kafka.

#include <iostream>
#include <string>

#include "app/sim_config_applier.hpp"
#include "app/sim_session_registry.hpp"

namespace {

using cex::venues::app::ApplySimConfigEvent;
using cex::venues::app::SimConfigAction;
using cex::venues::app::SimSessionRegistry;

namespace simv1 = fob::sim::v1;

bool Check(bool cond, const std::string& msg) {
  if (cond) return true;
  std::cerr << "[FAIL] " << msg << std::endl;
  return false;
}

simv1::SimConfigEvent MakeEvent(simv1::SimConfigEventType type,
                                const std::string& id, simv1::RoutingMode mode,
                                simv1::SimSessionStatus status) {
  simv1::SimConfigEvent evt;
  evt.set_event_type(type);
  auto* s = evt.mutable_session();
  s->set_sim_session_id(id);
  s->set_routing_mode(mode);
  s->set_status(status);
  return evt;
}

// 1. UPSERT ACTIVE -> registry resolves it.
bool test_upsert_active() {
  SimSessionRegistry reg;
  auto act = ApplySimConfigEvent(
      MakeEvent(simv1::SIM_CONFIG_EVENT_TYPE_UPSERT, "s1",
                simv1::ROUTING_MODE_SIM_ONLY, simv1::SIM_SESSION_STATUS_ACTIVE),
      reg);
  bool ok = true;
  ok &= Check(act == SimConfigAction::kUpsert, "upsert: action");
  ok &= Check(reg.Resolve("any", "any").has_value(), "upsert: resolvable (wildcard)");
  return ok;
}

// 2. Hot reload: second UPSERT for same id replaces routing mode.
bool test_hot_reload() {
  SimSessionRegistry reg;
  ApplySimConfigEvent(
      MakeEvent(simv1::SIM_CONFIG_EVENT_TYPE_UPSERT, "s2",
                simv1::ROUTING_MODE_SIM_ONLY, simv1::SIM_SESSION_STATUS_ACTIVE),
      reg);
  ApplySimConfigEvent(
      MakeEvent(simv1::SIM_CONFIG_EVENT_TYPE_UPSERT, "s2",
                simv1::ROUTING_MODE_SHADOW, simv1::SIM_SESSION_STATUS_ACTIVE),
      reg);
  auto s = reg.Resolve("v", "i");
  bool ok = true;
  ok &= Check(reg.Size() == 1, "hot_reload: still one session");
  ok &= Check(s.has_value() && s->routing_mode() == simv1::ROUTING_MODE_SHADOW,
              "hot_reload: mode updated to SHADOW");
  return ok;
}

// 3. DELETE removes.
bool test_delete() {
  SimSessionRegistry reg;
  ApplySimConfigEvent(
      MakeEvent(simv1::SIM_CONFIG_EVENT_TYPE_UPSERT, "s3",
                simv1::ROUTING_MODE_SIM_ONLY, simv1::SIM_SESSION_STATUS_ACTIVE),
      reg);
  auto act = ApplySimConfigEvent(
      MakeEvent(simv1::SIM_CONFIG_EVENT_TYPE_DELETE, "s3",
                simv1::ROUTING_MODE_SIM_ONLY, simv1::SIM_SESSION_STATUS_ACTIVE),
      reg);
  bool ok = true;
  ok &= Check(act == SimConfigAction::kDelete, "delete: action");
  ok &= Check(reg.Size() == 0, "delete: removed");
  return ok;
}

// 4. Unspecified event type -> ignored, registry untouched.
bool test_unspecified_ignored() {
  SimSessionRegistry reg;
  ApplySimConfigEvent(
      MakeEvent(simv1::SIM_CONFIG_EVENT_TYPE_UPSERT, "keep",
                simv1::ROUTING_MODE_SIM_ONLY, simv1::SIM_SESSION_STATUS_ACTIVE),
      reg);
  auto act = ApplySimConfigEvent(
      MakeEvent(simv1::SIM_CONFIG_EVENT_TYPE_UNSPECIFIED, "x",
                simv1::ROUTING_MODE_SIM_ONLY, simv1::SIM_SESSION_STATUS_ACTIVE),
      reg);
  bool ok = true;
  ok &= Check(act == SimConfigAction::kIgnored, "unspecified: ignored");
  ok &= Check(reg.Size() == 1, "unspecified: untouched");
  return ok;
}

// 5. Empty session id -> ignored.
bool test_empty_id_ignored() {
  SimSessionRegistry reg;
  auto act = ApplySimConfigEvent(
      MakeEvent(simv1::SIM_CONFIG_EVENT_TYPE_UPSERT, "",
                simv1::ROUTING_MODE_SIM_ONLY, simv1::SIM_SESSION_STATUS_ACTIVE),
      reg);
  return Check(act == SimConfigAction::kIgnored && reg.Size() == 0,
               "empty id: ignored, empty registry");
}

// 6. DELETE for unknown id -> no crash, registry stays empty.
bool test_delete_nonexistent() {
  SimSessionRegistry reg;
  auto act = ApplySimConfigEvent(
      MakeEvent(simv1::SIM_CONFIG_EVENT_TYPE_DELETE, "ghost",
                simv1::ROUTING_MODE_SIM_ONLY, simv1::SIM_SESSION_STATUS_ACTIVE),
      reg);
  return Check(act == SimConfigAction::kDelete && reg.Size() == 0,
               "delete nonexistent: safe no-op");
}

// 7. UPSERT PAUSED -> stored but not active.
bool test_upsert_paused() {
  SimSessionRegistry reg;
  ApplySimConfigEvent(
      MakeEvent(simv1::SIM_CONFIG_EVENT_TYPE_UPSERT, "s7",
                simv1::ROUTING_MODE_SIM_ONLY, simv1::SIM_SESSION_STATUS_PAUSED),
      reg);
  bool ok = true;
  ok &= Check(reg.Size() == 1 && reg.ActiveCount() == 0, "paused: stored not active");
  ok &= Check(!reg.Resolve("v", "i").has_value(), "paused: not resolved");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) { std::cerr << "  in test: " << name << std::endl; ok = false; }
  };

  run("upsert_active", test_upsert_active);
  run("hot_reload", test_hot_reload);
  run("delete", test_delete);
  run("unspecified_ignored", test_unspecified_ignored);
  run("empty_id_ignored", test_empty_id_ignored);
  run("delete_nonexistent", test_delete_nonexistent);
  run("upsert_paused", test_upsert_paused);

  if (ok) {
    std::cout << "[OK] sim_config_applier_test passed (7 cases: upsert, hot "
                 "reload, delete, unspecified/empty ignored, delete-nonexistent, "
                 "paused)."
              << std::endl;
    return 0;
  }
  return 1;
}
