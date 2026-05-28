#pragma once

#include <cstdint>
#include <string>

#include "app/venue_sim_router.hpp"  // RouteDecision (models, scope, session)
#include "app/venue_simulator.hpp"   // VenueSimulator
#include "fob/execution/v1/execution.pb.h"
#include "fob/sim/v1/sim.pb.h"
#include "fob/venue/v1/venue.pb.h"

namespace cex::venues::app {

// F-20 Phase 4 (T-F20-403, data-plane core). Given a routed child order and
// the live LOB, runs the VenueSimulator and assembles the two correlated
// messages a SIM / SHADOW route emits:
//
//   - ExecutionReport — IDENTICAL contract to the LIVE path (ADR-015), to be
//     published to `sim.execution.venue`. Mirrors execute_on_venue's
//     make_execution_report, driven by the simulated fill instead of a real
//     venue response, and carries the F-12 correlation fields
//     (hedge_flow_id / child_order_id / batch_id / provider_id) from the
//     intent.
//   - SimExecutionAnnotation — sim provenance sidecar, to be published to
//     `sim.execution.annotations`. Correlated to the report by report_id.
//
// PURE: no Kafka, no threads, no async wait. The LatencyModel value is
// SAMPLED into the annotation (latency_sample_ms); the runtime caller applies
// the actual delay before publishing. Deterministic given rng_seed.

struct SimExecutionInputs {
  fob::execution::v1::ExecutionIntent intent;
  const fob::venue::v1::VenueSnapshot* snapshot{nullptr};
  uint32_t lob_age_ms{0};
  uint64_t rng_seed{0};
};

struct SimExecutionOutput {
  fob::execution::v1::ExecutionReport report;
  fob::sim::v1::SimExecutionAnnotation annotation;
};

class SimExecutionAssembler {
 public:
  // `decision` supplies the SimModels, stale-LOB threshold, partial-fill mode
  // and sim_session_id (from VenueSimRouter::Decide).
  SimExecutionOutput Assemble(const SimExecutionInputs& inputs,
                              const RouteDecision& decision) const;

 private:
  VenueSimulator simulator_;
};

}  // namespace cex::venues::app
