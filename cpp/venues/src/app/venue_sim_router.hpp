#pragma once

#include <cstdint>
#include <string>

#include "app/sim_session_registry.hpp"
#include "app/venue_simulator.hpp"
#include "fob/sim/v1/sim.pb.h"

namespace cex::venues::app {

// F-20 Phase 4 (T-F20-402). VenueSimRouter sits between the Venue Execution
// Adapter and the choice of execution target. Given a child order's
// (venue_id, symbol) it decides — from the active SimSessions in the
// registry — whether the order goes to the real EVC (LIVE_ONLY), the
// VenueSimulator (SIM_ONLY), or both (SHADOW), and surfaces the SimModels
// needed to drive the simulator.
//
// This PR provides the pure DECISION + model extraction. The actual
// execution fork — calling VenueSimulator, applying the async latency delay,
// publishing the sim ExecutionReport to `sim.execution.venue` plus the
// SimExecutionAnnotation sidecar, and SimAlert emission — is a later wiring
// PR. Keeping the decision pure makes it deterministic and unit-testable
// without Kafka or threads.

struct RouteDecision {
  // Resolved routing mode. Defaults to LIVE_ONLY when no ACTIVE session
  // governs this (venue, instrument): the simulator is strictly opt-in, so
  // an order never silently routes to sim.
  fob::sim::v1::RoutingMode mode{fob::sim::v1::ROUTING_MODE_LIVE_ONLY};

  bool has_session{false};
  std::string sim_session_id;

  // Valid when has_session && RoutesToSim().
  SimModels models;
  uint32_t stale_lob_threshold_ms{2000};
  fob::sim::v1::PartialFillMode partial_fill_mode{
      fob::sim::v1::PARTIAL_FILL_MODE_LEVEL_BY_LEVEL};

  bool RoutesToSim() const {
    return mode == fob::sim::v1::ROUTING_MODE_SIM_ONLY ||
           mode == fob::sim::v1::ROUTING_MODE_SHADOW;
  }
  bool RoutesToLive() const {
    return mode == fob::sim::v1::ROUTING_MODE_LIVE_ONLY ||
           mode == fob::sim::v1::ROUTING_MODE_SHADOW;
  }
};

class VenueSimRouter {
 public:
  explicit VenueSimRouter(const SimSessionRegistry& registry)
      : registry_(registry) {}

  RouteDecision Decide(const std::string& venue_id,
                       const std::string& symbol) const;

 private:
  const SimSessionRegistry& registry_;
};

}  // namespace cex::venues::app
