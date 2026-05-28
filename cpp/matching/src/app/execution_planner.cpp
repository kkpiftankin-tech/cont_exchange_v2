#include "app/execution_planner.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "cex/common/log.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

namespace cex::matching::app {

namespace {

// Largest quantity grid point on the side, i.e. how much volume we can
// absorb from this venue's curve at the worst price tier.
double MaxQtyOnSide(const fob::venue::v1::SideLiquidityCurve& side) {
  double max_q = 0.0;
  for (double q : side.q_grid()) {
    if (std::isfinite(q) && q > max_q) max_q = q;
  }
  return max_q;
}

// Pick the side relevant to executing a BUY (we hit asks) vs SELL
// (we hit bids).
const fob::venue::v1::SideLiquidityCurve& SideCurveForIntent(
    const fob::venue::v1::VenueLiquidityCurve& curve,
    fob::common::v1::Side side) {
  return side == fob::common::v1::SIDE_BUY ? curve.ask_curve() : curve.bid_curve();
}

bool VenueHealthGreen(const fob::venue::v1::VenueHealth& health) {
  if (health.routing_recommendation() == fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK) return false;
  if (health.breaker_state() == fob::venue::v1::CIRCUIT_BREAKER_STATE_OPEN) return false;
  return true;
}

cex::common::Decimal DecimalFromDouble(double value, int32_t scale = 8) {
  if (!std::isfinite(value) || value <= 0.0) return cex::common::Decimal::zero();
  double scaled = value;
  for (int32_t i = 0; i < scale; ++i) scaled *= 10.0;
  int64_t units = static_cast<int64_t>(std::llround(scaled));
  if (units < 0) units = 0;
  return cex::common::Decimal{units, scale};
}

double DecimalToDouble(const cex::common::Decimal& d) {
  return static_cast<double>(d);
}

}  // namespace

PlanResult BuildMultiVenuePlan(const PlanRequest& request,
                               const PlanConfig& config) {
  PlanResult out;

  if (DecimalToDouble(request.target_qty) <= 0.0) {
    out.reject_reason = "target_qty_non_positive";
    return out;
  }

  const std::unordered_set<std::string> allowed(
      request.allowed_venues.begin(), request.allowed_venues.end());
  const bool has_allow_list = !allowed.empty();

  // Pass 1: filter by usability + health + allow-list + extract L(v).
  struct Candidate {
    std::string venue_id;
    double liquidity{0.0};
  };
  std::vector<Candidate> candidates;
  candidates.reserve(request.planner_inputs.size());

  for (const auto& input : request.planner_inputs) {
    if (!input.usable) continue;
    if (has_allow_list && allowed.find(input.venue_id) == allowed.end()) continue;
    if (input.health.has_value() && !VenueHealthGreen(*input.health)) continue;
    // NOTE: do NOT filter on input.curve.instrument().symbol() — the
    // snapshot is already symbol-scoped by PlannerInputsCache's CurveKey,
    // and the embedded curve.instrument may carry the venue-normalized
    // symbol (e.g. WBTCUSDC on uniswap) rather than the internal symbol.

    const double liquidity = MaxQtyOnSide(SideCurveForIntent(input.curve, request.side));
    if (!(liquidity > 0.0)) continue;

    candidates.push_back({input.venue_id, liquidity});
  }

  if (candidates.empty()) {
    out.reject_reason = "no_usable_venue";
    return out;
  }

  // Pass 2: trivial single-venue case.
  if (candidates.size() == 1) {
    VenueAllocation alloc;
    alloc.venue_id = candidates[0].venue_id;
    alloc.qty = request.target_qty;
    alloc.liquidity_used = candidates[0].liquidity;
    alloc.share = 1.0;
    out.allocations.push_back(std::move(alloc));
    out.feasible = true;
    return out;
  }

  // Pass 3: proportional split.
  double total_liquidity = 0.0;
  for (const auto& c : candidates) total_liquidity += c.liquidity;
  if (!(total_liquidity > 0.0)) {
    out.reject_reason = "zero_total_liquidity";
    return out;
  }

  const double target_qty_d = DecimalToDouble(request.target_qty);
  const double min_alloc_threshold = std::max(
      config.min_allocation_abs,
      config.min_allocation_fraction * target_qty_d);

  struct Provisional {
    std::string venue_id;
    double liquidity;
    double share;
    double qty;
  };
  std::vector<Provisional> provisional;
  provisional.reserve(candidates.size());
  for (const auto& c : candidates) {
    const double share = c.liquidity / total_liquidity;
    provisional.push_back({c.venue_id, c.liquidity, share, share * target_qty_d});
  }

  // Pass 4: drop sub-threshold allocations, redistribute their share
  // proportionally across surviving venues.
  std::vector<Provisional> kept;
  double dropped_share = 0.0;
  for (const auto& p : provisional) {
    if (p.qty < min_alloc_threshold) {
      dropped_share += p.share;
    } else {
      kept.push_back(p);
    }
  }

  if (kept.empty()) {
    // Threshold too strict — fall back to single-venue (largest L).
    auto best = std::max_element(
        provisional.begin(), provisional.end(),
        [](const Provisional& a, const Provisional& b) {
          return a.liquidity < b.liquidity;
        });
    VenueAllocation alloc;
    alloc.venue_id = best->venue_id;
    alloc.qty = request.target_qty;
    alloc.liquidity_used = best->liquidity;
    alloc.share = 1.0;
    out.allocations.push_back(std::move(alloc));
    out.feasible = true;
    return out;
  }

  if (dropped_share > 0.0) {
    const double kept_share_sum = 1.0 - dropped_share;
    if (kept_share_sum > 0.0) {
      for (auto& p : kept) {
        p.share /= kept_share_sum;
        p.qty = p.share * target_qty_d;
      }
    }
  }

  // Pass 5: rounding fix — the sum of rounded qtys may drift by a
  // few units of the last decimal. Adjust the largest allocation so
  // the total equals target_qty exactly.
  std::vector<cex::common::Decimal> qtys;
  qtys.reserve(kept.size());
  cex::common::Decimal sum_so_far = cex::common::Decimal::zero();
  for (const auto& p : kept) {
    qtys.push_back(DecimalFromDouble(p.qty));
    sum_so_far = cex::common::Decimal::add(sum_so_far, qtys.back());
  }
  const auto diff = cex::common::Decimal::sub(request.target_qty, sum_so_far);
  if (cex::common::Decimal::cmp(diff, cex::common::Decimal::zero()) != 0) {
    // attribute the residual to the largest provisional allocation
    std::size_t largest_idx = 0;
    for (std::size_t i = 1; i < kept.size(); ++i) {
      if (kept[i].qty > kept[largest_idx].qty) largest_idx = i;
    }
    qtys[largest_idx] = cex::common::Decimal::add(qtys[largest_idx], diff);
  }

  out.allocations.reserve(kept.size());
  for (std::size_t i = 0; i < kept.size(); ++i) {
    VenueAllocation alloc;
    alloc.venue_id = kept[i].venue_id;
    alloc.qty = qtys[i];
    alloc.liquidity_used = kept[i].liquidity;
    alloc.share = kept[i].share;
    out.allocations.push_back(std::move(alloc));
  }
  out.feasible = true;
  return out;
}

std::vector<fob::execution::v1::ExecutionIntent> FanOutIntentByPlan(
    const fob::execution::v1::ExecutionIntent& intent,
    const PlanResult& plan) {
  std::vector<fob::execution::v1::ExecutionIntent> out;
  if (!plan.feasible || plan.allocations.empty()) {
    out.push_back(intent);
    return out;
  }
  if (plan.allocations.size() == 1 && plan.allocations[0].venue_id.empty()) {
    out.push_back(intent);
    return out;
  }

  out.reserve(plan.allocations.size());
  const std::string& base_hedge_flow_id = intent.hedge_flow_id();

  for (const auto& alloc : plan.allocations) {
    fob::execution::v1::ExecutionIntent clone = intent;

    // hedge_flow_id and intent_id get a venue suffix so each fan-out
    // child gets its own PG hedgeflows row.
    const std::string new_hedge_flow_id =
        base_hedge_flow_id.empty()
            ? std::string{"hedge|"} + alloc.venue_id
            : base_hedge_flow_id + "|" + alloc.venue_id;
    const std::string new_intent_id = new_hedge_flow_id + "|intent";

    clone.set_hedge_flow_id(new_hedge_flow_id);
    clone.set_intent_id(new_intent_id);
    clone.set_client_order_id(new_intent_id);
    clone.set_venue(alloc.venue_id);

    // allocated qty
    *clone.mutable_target_qty() = alloc.qty.to_proto();
    if (intent.has_reference_mid()) {
      const auto target_notional = cex::common::Decimal::mul(
          alloc.qty, cex::common::Decimal::from_proto(intent.reference_mid()));
      *clone.mutable_target_notional() = target_notional.to_proto();
    }

    // lock allowed_venues to the single chosen one
    clone.clear_allowed_venues();
    clone.add_allowed_venues(alloc.venue_id);

    // refresh meta
    auto* meta = clone.mutable_meta();
    meta->set_event_id(new_intent_id);
    *meta->mutable_ts_event() = cex::common::now_ts();
    meta->set_partition_key(intent.provider_id() + "|" + intent.instrument().symbol());

    out.push_back(std::move(clone));
  }

  return out;
}

}  // namespace cex::matching::app
