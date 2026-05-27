#pragma once

#include <string>
#include <vector>

#include "app/planner_inputs_cache.hpp"
#include "cex/common/decimal.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/execution/v1/execution.pb.h"

namespace cex::matching::app {

// F-12 DoD-2 (PR-F12-15): one allocation slot of the multi-venue
// routing plan for a single ExecutionIntent.
//
// qty = (liquidity[v] / sum_liquidity) * target_qty
//
// Slot is suppressed if the per-venue qty would be below
// min_allocation_qty (avoid dust orders that breach min_lot
// constraints on venues).
struct VenueAllocation {
  std::string venue_id;
  cex::common::Decimal qty{cex::common::Decimal::zero()};
  double liquidity_used{0.0};          // L(v) from the curve (max q_grid)
  double share{0.0};                   // L(v) / Sum(L(v')), in [0,1]
};

struct PlanRequest {
  std::string symbol;
  fob::common::v1::Side side{fob::common::v1::SIDE_BUY};
  cex::common::Decimal target_qty{cex::common::Decimal::zero()};
  // List of venues that matching's HEDGE_INTENT_ALLOWED_VENUES env
  // permits for this intent. Acts as a hard filter — venues outside
  // the list never receive any allocation, even if their curves are
  // healthier.
  std::vector<std::string> allowed_venues;
  // Snapshot of planner inputs for `symbol` taken from
  // PlannerInputsCache::GetPlannerInputsSnapshotForSymbol.
  std::vector<PlannerVenueInput> planner_inputs;
};

struct PlanConfig {
  // Min per-venue allocation qty as a fraction of target_qty
  // (0.05 = 5%). Allocations below this are dropped and their
  // share is redistributed proportionally to the remaining venues.
  double min_allocation_fraction{0.05};
  // Absolute minimum allocation in base units. If both this and
  // min_allocation_fraction kick in, the larger one wins.
  double min_allocation_abs{0.0};
};

struct PlanResult {
  std::vector<VenueAllocation> allocations;
  std::string reject_reason;            // empty on success
  bool feasible{false};                 // false when no venue can serve
};

// Build a multi-venue routing plan. Algorithm:
//   1. Filter planner_inputs by allowed_venues (intersection).
//   2. Drop entries with usable=false OR circuit_breaker_open.
//   3. For each survivor, compute L(v) = max(q_grid) on the side
//      the intent will hit (BUY -> ask_curve, SELL -> bid_curve).
//   4. Drop venues with L(v) <= 0.
//   5. If no survivor — feasible=false, reject_reason="no_usable_venue".
//   6. If exactly 1 survivor — single allocation = target_qty.
//   7. If >=2 survivors — allocate proportionally, then prune
//      below-threshold allocations and renormalise the remainder so
//      the sum stays equal to target_qty.
//
// The function never throws and never mutates its inputs.
PlanResult BuildMultiVenuePlan(const PlanRequest& request,
                               const PlanConfig& config = PlanConfig{});

// Apply a plan to a single ExecutionIntent: produce N intent clones,
// one per allocation. Each clone has:
//   - intent.venue        = allocation.venue_id
//   - intent.target_qty   = allocation.qty
//   - intent.hedge_flow_id= "<original_hedge_flow_id>|<venue_id>"
//   - intent.intent_id    = "<new_hedge_flow_id>|intent"
//   - intent.client_order_id = intent.intent_id  (matches existing convention)
//   - intent.allowed_venues = [venue_id]  (lock to chosen venue)
//   - meta.event_id / meta.partition_key updated to match the new ids
//
// When `plan.allocations.size() <= 1` the original intent is returned
// unchanged (single-element vector) so callers can always use the
// result identically.
std::vector<fob::execution::v1::ExecutionIntent> FanOutIntentByPlan(
    const fob::execution::v1::ExecutionIntent& intent,
    const PlanResult& plan);

}  // namespace cex::matching::app
