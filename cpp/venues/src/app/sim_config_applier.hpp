#pragma once

#include "app/sim_session_registry.hpp"
#include "fob/sim/v1/sim.pb.h"

namespace cex::venues::app {

// F-20 Phase 4 (T-F20-401). Pure mapping of one `sim.config` SimConfigEvent to
// a SimSessionRegistry mutation — the runtime hot-reload bridge for
// VenueSimRouter. Kept separate from the Kafka consume loop so the
// event->action semantics are deterministic and unit-testable without Kafka.
enum class SimConfigAction { kIgnored, kUpsert, kDelete };

const char* ToString(SimConfigAction action);

// UPSERT -> registry.Upsert(session)  (create / hot-reload update / pause).
// DELETE -> registry.Remove(id)       (session completed / cancelled).
// Unspecified event type or empty sim_session_id -> kIgnored (untouched).
SimConfigAction ApplySimConfigEvent(const fob::sim::v1::SimConfigEvent& evt,
                                    SimSessionRegistry& registry);

}  // namespace cex::venues::app
