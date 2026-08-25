#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "app/execution_planner.hpp"
#include "app/planner_inputs_cache.hpp"

namespace {

using cex::common::Decimal;
using cex::matching::app::BuildMultiVenuePlan;
using cex::matching::app::FanOutIntentByPlan;
using cex::matching::app::PlanConfig;
using cex::matching::app::PlannerVenueInput;
using cex::matching::app::PlanRequest;

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

// Build a PlannerVenueInput with a synthetic curve whose ask/bid q_grid
// max equals `liquidity`. usable + green health by default.
PlannerVenueInput MakeInput(const std::string& venue_id,
                            const std::string& symbol,
                            double liquidity,
                            bool usable = true,
                            bool circuit_open = false) {
  PlannerVenueInput in;
  in.venue_id = venue_id;
  in.symbol = symbol;
  in.usable = usable;

  auto* instrument = in.curve.mutable_instrument();
  instrument->set_symbol(symbol);

  // Populate both sides so BUY (ask) and SELL (bid) both see liquidity.
  auto* ask = in.curve.mutable_ask_curve();
  ask->add_q_grid(liquidity * 0.5);
  ask->add_q_grid(liquidity);
  auto* bid = in.curve.mutable_bid_curve();
  bid->add_q_grid(liquidity * 0.5);
  bid->add_q_grid(liquidity);

  fob::venue::v1::VenueHealth health;
  health.set_routing_recommendation(
      circuit_open ? fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK
                   : fob::venue::v1::ROUTING_RECOMMENDATION_ALLOW);
  health.set_breaker_state(circuit_open
                               ? fob::venue::v1::CIRCUIT_BREAKER_STATE_OPEN
                               : fob::venue::v1::CIRCUIT_BREAKER_STATE_CLOSED);
  in.health = health;
  return in;
}

double AllocSum(const cex::matching::app::PlanResult& plan) {
  double sum = 0.0;
  for (const auto& a : plan.allocations) sum += static_cast<double>(a.qty);
  return sum;
}

// ---------------------------------------------------------------------------

bool TestProportionalSplitTwoVenues() {
  // binance L=3, uniswap L=1 -> shares 0.75 / 0.25 of target 0.4.
  PlanRequest req;
  req.symbol = "BTC/USDT";
  req.side = fob::common::v1::SIDE_BUY;       // hits ask
  req.target_qty = Decimal{40, 2};            // 0.40
  req.allowed_venues = {"binance", "uniswap_v3"};
  req.planner_inputs = {
      MakeInput("binance", "BTC/USDT", 3.0),
      MakeInput("uniswap_v3", "BTC/USDT", 1.0),
  };

  const auto plan = BuildMultiVenuePlan(req);
  bool ok = true;
  ok = Expect(plan.feasible, "two-venue plan feasible") && ok;
  ok = Expect(plan.allocations.size() == 2, "two allocations") && ok;
  // sum equals target exactly (rounding residual reconciled)
  ok = Expect(std::abs(AllocSum(plan) - 0.40) < 1e-9, "alloc sum == target") && ok;
  // binance should get ~0.30, uniswap ~0.10
  double binance_qty = 0.0, uni_qty = 0.0;
  for (const auto& a : plan.allocations) {
    if (a.venue_id == "binance") binance_qty = static_cast<double>(a.qty);
    if (a.venue_id == "uniswap_v3") uni_qty = static_cast<double>(a.qty);
  }
  ok = Expect(std::abs(binance_qty - 0.30) < 1e-6, "binance ~0.30") && ok;
  ok = Expect(std::abs(uni_qty - 0.10) < 1e-6, "uniswap ~0.10") && ok;
  return ok;
}

bool TestCircuitOpenVenueExcluded() {
  // coinbase circuit open -> dropped; only binance survives -> single alloc.
  PlanRequest req;
  req.symbol = "BTC/USDT";
  req.side = fob::common::v1::SIDE_BUY;
  req.target_qty = Decimal{50, 3};            // 0.050
  req.allowed_venues = {"binance", "coinbase"};
  req.planner_inputs = {
      MakeInput("binance", "BTC/USDT", 2.0),
      MakeInput("coinbase", "BTC/USDT", 5.0, /*usable=*/true, /*circuit_open=*/true),
  };

  const auto plan = BuildMultiVenuePlan(req);
  bool ok = true;
  ok = Expect(plan.feasible, "circuit-test feasible") && ok;
  ok = Expect(plan.allocations.size() == 1, "single alloc after circuit drop") && ok;
  ok = Expect(plan.allocations[0].venue_id == "binance", "binance survives") && ok;
  // single venue gets the full target
  ok = Expect(std::abs(AllocSum(plan) - 0.050) < 1e-9, "full target to survivor") && ok;
  return ok;
}

bool TestAllowListFiltersVenue() {
  // allow-list excludes uniswap even though it has the most liquidity.
  PlanRequest req;
  req.symbol = "BTC/USDT";
  req.side = fob::common::v1::SIDE_SELL;       // hits bid
  req.target_qty = Decimal{10, 2};
  req.allowed_venues = {"binance"};
  req.planner_inputs = {
      MakeInput("binance", "BTC/USDT", 1.0),
      MakeInput("uniswap_v3", "BTC/USDT", 9.0),
  };

  const auto plan = BuildMultiVenuePlan(req);
  bool ok = true;
  ok = Expect(plan.allocations.size() == 1, "allow-list narrows to 1") && ok;
  ok = Expect(plan.allocations.empty() || plan.allocations[0].venue_id == "binance",
              "only binance allowed") && ok;
  return ok;
}

bool TestNoUsableVenueInfeasible() {
  PlanRequest req;
  req.symbol = "BTC/USDT";
  req.side = fob::common::v1::SIDE_BUY;
  req.target_qty = Decimal{10, 2};
  req.allowed_venues = {"binance"};
  req.planner_inputs = {
      MakeInput("binance", "BTC/USDT", 0.0),  // zero liquidity -> dropped
  };

  const auto plan = BuildMultiVenuePlan(req);
  bool ok = true;
  ok = Expect(!plan.feasible, "infeasible when no liquidity") && ok;
  ok = Expect(plan.allocations.empty(), "no allocations") && ok;
  ok = Expect(plan.reject_reason == "no_usable_venue", "reject reason set") && ok;
  return ok;
}

bool TestSubThresholdPruneRedistributes() {
  // 3 venues: L = 100, 100, 1. The tiny venue (~0.5% of target) is below
  // the 5% min_allocation_fraction -> pruned, its share redistributed to
  // the two big venues (which end up ~50/50).
  PlanRequest req;
  req.symbol = "BTC/USDT";
  req.side = fob::common::v1::SIDE_BUY;
  req.target_qty = Decimal{100, 1};            // 10.0
  req.allowed_venues = {"binance", "okx", "dust"};
  req.planner_inputs = {
      MakeInput("binance", "BTC/USDT", 100.0),
      MakeInput("okx", "BTC/USDT", 100.0),
      MakeInput("dust", "BTC/USDT", 1.0),
  };

  const auto plan = BuildMultiVenuePlan(req);
  bool ok = true;
  ok = Expect(plan.feasible, "prune-test feasible") && ok;
  ok = Expect(plan.allocations.size() == 2, "dust pruned -> 2 allocations") && ok;
  ok = Expect(std::abs(AllocSum(plan) - 10.0) < 1e-9, "sum still == target after prune") && ok;
  for (const auto& a : plan.allocations) {
    ok = Expect(a.venue_id != "dust", "dust venue excluded") && ok;
  }
  return ok;
}

bool TestFanOutClonesIntent() {
  PlanRequest req;
  req.symbol = "BTC/USDT";
  req.side = fob::common::v1::SIDE_BUY;
  req.target_qty = Decimal{40, 2};
  req.allowed_venues = {"binance", "uniswap_v3"};
  req.planner_inputs = {
      MakeInput("binance", "BTC/USDT", 3.0),
      MakeInput("uniswap_v3", "BTC/USDT", 1.0),
  };
  const auto plan = BuildMultiVenuePlan(req);

  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id("batch1|hedge|prov|BTC/USDT|intent");
  intent.set_hedge_flow_id("batch1|hedge|prov|BTC/USDT");
  intent.set_provider_id("prov");
  intent.mutable_instrument()->set_symbol("BTC/USDT");
  intent.set_side(fob::common::v1::SIDE_BUY);
  auto* ref = intent.mutable_reference_mid();
  ref->set_units(76000);
  ref->set_scale(0);

  const auto clones = FanOutIntentByPlan(intent, plan);
  bool ok = true;
  ok = Expect(clones.size() == 2, "fan-out yields 2 clones") && ok;
  for (const auto& c : clones) {
    // hedge_flow_id gets a venue suffix
    ok = Expect(c.hedge_flow_id().find('|' + c.venue()) != std::string::npos,
                "clone hedge_flow_id has venue suffix") && ok;
    // intent_id = hedge_flow_id|intent
    ok = Expect(c.intent_id() == c.hedge_flow_id() + "|intent",
                "clone intent_id derived") && ok;
    // allowed_venues locked to the single chosen venue
    ok = Expect(c.allowed_venues_size() == 1 && c.allowed_venues(0) == c.venue(),
                "allowed_venues locked") && ok;
    ok = Expect(c.client_order_id() == c.intent_id(), "client_order_id == intent_id") && ok;
  }
  return ok;
}

bool TestFanOutPassthroughSingle() {
  // single-allocation plan -> FanOut returns the original intent unchanged.
  PlanRequest req;
  req.symbol = "BTC/USDT";
  req.side = fob::common::v1::SIDE_BUY;
  req.target_qty = Decimal{5, 2};
  req.allowed_venues = {"binance"};
  req.planner_inputs = {MakeInput("binance", "BTC/USDT", 3.0)};
  const auto plan = BuildMultiVenuePlan(req);

  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id("orig|intent");
  intent.set_hedge_flow_id("orig");
  const auto clones = FanOutIntentByPlan(intent, plan);
  bool ok = true;
  // single venue plan: allocations==1, venue_id non-empty -> FanOut still
  // clones (1 element) with venue suffix. Assert exactly 1 element.
  ok = Expect(clones.size() == 1, "single alloc -> 1 element") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestProportionalSplitTwoVenues() && ok;
  ok = TestCircuitOpenVenueExcluded() && ok;
  ok = TestAllowListFiltersVenue() && ok;
  ok = TestNoUsableVenueInfeasible() && ok;
  ok = TestSubThresholdPruneRedistributes() && ok;
  ok = TestFanOutClonesIntent() && ok;
  ok = TestFanOutPassthroughSingle() && ok;
  if (ok) std::cout << "execution_planner_test: ALL PASS\n";
  return ok ? 0 : 1;
}
