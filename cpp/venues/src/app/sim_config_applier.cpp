#include "app/sim_config_applier.hpp"

namespace cex::venues::app {

const char* ToString(SimConfigAction action) {
  switch (action) {
    case SimConfigAction::kIgnored: return "ignored";
    case SimConfigAction::kUpsert:  return "upsert";
    case SimConfigAction::kDelete:  return "delete";
  }
  return "ignored";
}

SimConfigAction ApplySimConfigEvent(const fob::sim::v1::SimConfigEvent& evt,
                                    SimSessionRegistry& registry) {
  const auto& session = evt.session();
  if (session.sim_session_id().empty()) return SimConfigAction::kIgnored;

  switch (evt.event_type()) {
    case fob::sim::v1::SIM_CONFIG_EVENT_TYPE_UPSERT:
      registry.Upsert(session);
      return SimConfigAction::kUpsert;
    case fob::sim::v1::SIM_CONFIG_EVENT_TYPE_DELETE:
      registry.Remove(session.sim_session_id());
      return SimConfigAction::kDelete;
    default:
      return SimConfigAction::kIgnored;
  }
}

}  // namespace cex::venues::app
