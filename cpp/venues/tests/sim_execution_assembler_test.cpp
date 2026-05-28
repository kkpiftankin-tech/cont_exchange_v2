// F-20 SimExecutionAssembler unit suite (Phase 4, T-F20-403 data-plane core).
//
// Pure: runs VenueSimulator over inputs and assembles the sim ExecutionReport
// (same contract as LIVE) + SimExecutionAnnotation sidecar. Deterministic.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include "app/sim_execution_assembler.hpp"

namespace {

using cex::common::Decimal;
using cex::venues::app::RouteDecision;
using cex::venues::app::SimExecutionAssembler;
using cex::venues::app::SimExecutionInputs;
using cex::venues::app::SimExecutionOutput;
using cex::venues::app::SimModels;

namespace execv1 = fob::execution::v1;
namespace simv1 = fob::sim::v1;
namespace commonv1 = fob::common::v1;

bool Check(bool cond, const std::string& msg) {
  if (cond) return true;
  std::cerr << "[FAIL] " << msg << std::endl;
  return false;
}

commonv1::Decimal PD(double v, int32_t scale = 8) {
  commonv1::Decimal d;
  double s = v;
  for (int i = 0; i < scale; ++i) s *= 10.0;
  d.set_units(static_cast<int64_t>(std::llround(s)));
  d.set_scale(scale);
  return d;
}

double D2(const commonv1::Decimal& d) {
  return static_cast<double>(Decimal::from_proto(d));
}

fob::venue::v1::VenueSnapshot MakeSnapshot() {
  fob::venue::v1::VenueSnapshot s;
  s.mutable_meta()->set_event_id("snap-7");
  s.set_venue_id("binance");
  s.mutable_instrument()->set_symbol("BTC/USDT");
  *s.mutable_best_bid() = PD(99.0);
  *s.mutable_best_ask() = PD(101.0);
  *s.mutable_mid_price() = PD(100.0);
  *s.add_ask_prices() = PD(101.0); *s.add_ask_quantities() = PD(1.0);
  *s.add_ask_prices() = PD(102.0); *s.add_ask_quantities() = PD(2.0);
  *s.add_ask_prices() = PD(103.0); *s.add_ask_quantities() = PD(3.0);
  *s.add_bid_prices() = PD(99.0); *s.add_bid_quantities() = PD(1.0);
  *s.add_bid_prices() = PD(98.0); *s.add_bid_quantities() = PD(2.0);
  return s;
}

SimModels NoOpModels() {
  SimModels m;
  m.latency.set_distribution(simv1::LATENCY_DISTRIBUTION_FIXED);
  m.latency.set_p50_ms(10);
  m.latency.set_timeout_ms(0);
  m.impact.set_model_type(simv1::IMPACT_MODEL_TYPE_LEVEL_BY_LEVEL);
  m.fee.set_taker_bps(0);
  m.fee.set_maker_bps(0);
  return m;
}

RouteDecision SimDecision(SimModels models) {
  RouteDecision d;
  d.mode = simv1::ROUTING_MODE_SIM_ONLY;
  d.has_session = true;
  d.sim_session_id = "sess-7";
  d.models = std::move(models);
  d.stale_lob_threshold_ms = 2000;
  d.partial_fill_mode = simv1::PARTIAL_FILL_MODE_LEVEL_BY_LEVEL;
  return d;
}

execv1::ExecutionIntent MakeIntent(double qty) {
  execv1::ExecutionIntent in;
  in.mutable_meta()->set_correlation_id("corr-7");
  in.set_intent_id("intent-7");
  in.set_hedge_flow_id("hf-7");
  in.set_batch_id("batch-7");
  in.set_provider_id("prov-7");
  in.set_venue("binance");
  in.mutable_instrument()->set_symbol("BTC/USDT");
  in.set_venue_symbol("BTCUSDT");
  in.set_side(commonv1::SIDE_BUY);
  *in.mutable_target_qty() = PD(qty);
  in.set_strategy(execv1::EXEC_STRATEGY_MARKET);
  in.set_client_order_id("clord-7");
  return in;
}

SimExecutionInputs MakeInputs(const fob::venue::v1::VenueSnapshot& snap,
                              const execv1::ExecutionIntent& intent,
                              uint32_t lob_age_ms) {
  SimExecutionInputs in;
  in.intent = intent;
  in.snapshot = &snap;
  in.lob_age_ms = lob_age_ms;
  in.rng_seed = 42;
  return in;
}

// 1. Filled order -> report mirrors LIVE contract + F-12 correlation fields.
bool test_report_fields_filled() {
  auto snap = MakeSnapshot();
  SimExecutionAssembler asmblr;
  auto out = asmblr.Assemble(MakeInputs(snap, MakeIntent(0.5), 100),
                             SimDecision(NoOpModels()));
  const auto& r = out.report;
  bool ok = true;
  ok &= Check(!r.report_id().empty(), "report_id set");
  ok &= Check(r.intent_id() == "intent-7", "intent_id");
  ok &= Check(r.hedge_flow_id() == "hf-7", "hedge_flow_id carried");
  ok &= Check(r.child_order_id() == "clord-7", "child_order_id = client_order_id");
  ok &= Check(r.batch_id() == "batch-7", "batch_id carried");
  ok &= Check(r.provider_id() == "prov-7", "provider_id carried");
  ok &= Check(r.venue() == "binance", "venue");
  ok &= Check(r.side() == commonv1::SIDE_BUY, "side");
  ok &= Check(r.client_order_id() == "clord-7", "client_order_id");
  ok &= Check(r.status() == execv1::EXECUTION_REPORT_STATUS_FILLED, "status FILLED");
  ok &= Check(std::abs(D2(r.filled_qty()) - 0.5) < 1e-9, "filled_qty 0.5");
  ok &= Check(r.meta().source() == "venue-simulator", "meta.source");
  ok &= Check(r.meta().correlation_id() == "corr-7", "correlation_id");
  return ok;
}

// 2. Annotation sidecar correlates to the report by report_id.
bool test_annotation_correlation() {
  auto snap = MakeSnapshot();
  SimExecutionAssembler asmblr;
  auto out = asmblr.Assemble(MakeInputs(snap, MakeIntent(0.5), 100),
                             SimDecision(NoOpModels()));
  const auto& a = out.annotation;
  bool ok = true;
  ok &= Check(a.report_id() == out.report.report_id(),
              "annotation.report_id == report.report_id");
  ok &= Check(a.sim_session_id() == "sess-7", "annotation sim_session_id");
  ok &= Check(a.lob_age_ms() == 100, "annotation lob_age_ms");
  ok &= Check(a.latency_sample_ms() == 10, "annotation latency (FIXED p50=10)");
  ok &= Check(a.meta().source() == "venue-simulator", "annotation meta.source");
  return ok;
}

// 3. Determinism: same inputs+seed -> same numeric fill + latency.
bool test_determinism() {
  auto snap = MakeSnapshot();
  SimExecutionAssembler asmblr;
  auto a = asmblr.Assemble(MakeInputs(snap, MakeIntent(0.5), 100),
                           SimDecision(NoOpModels()));
  auto b = asmblr.Assemble(MakeInputs(snap, MakeIntent(0.5), 100),
                           SimDecision(NoOpModels()));
  bool ok = true;
  ok &= Check(a.report.filled_qty().units() == b.report.filled_qty().units(),
              "determinism: filled_qty");
  ok &= Check(a.report.average_price().units() == b.report.average_price().units(),
              "determinism: avg_price");
  ok &= Check(a.annotation.latency_sample_ms() == b.annotation.latency_sample_ms(),
              "determinism: latency");
  return ok;
}

// 4. Stale LOB -> REJECTED report with SIM_STALE_LOB error, no fill.
bool test_stale_reject() {
  auto snap = MakeSnapshot();
  SimExecutionAssembler asmblr;
  auto out = asmblr.Assemble(MakeInputs(snap, MakeIntent(0.5), 5000),
                             SimDecision(NoOpModels()));
  const auto& r = out.report;
  bool ok = true;
  ok &= Check(r.status() == execv1::EXECUTION_REPORT_STATUS_REJECTED,
              "stale: REJECTED");
  ok &= Check(r.error().code() == "SIM_STALE_LOB", "stale: error code");
  ok &= Check(std::abs(D2(r.filled_qty())) < 1e-9, "stale: no fill");
  return ok;
}

// 5. Taker fee model -> fee_total populated on the report.
bool test_fee_propagation() {
  auto snap = MakeSnapshot();
  SimModels m = NoOpModels();
  m.fee.set_taker_bps(10);  // 10 bps
  SimExecutionAssembler asmblr;
  auto out = asmblr.Assemble(MakeInputs(snap, MakeIntent(0.5), 100),
                             SimDecision(m));
  const auto& r = out.report;
  bool ok = true;
  ok &= Check(r.has_fee_total(), "fee: fee_total present");
  ok &= Check(r.fee_total().cost().amount().units() != 0, "fee: nonzero cost");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) { std::cerr << "  in test: " << name << std::endl; ok = false; }
  };

  run("report_fields_filled", test_report_fields_filled);
  run("annotation_correlation", test_annotation_correlation);
  run("determinism", test_determinism);
  run("stale_reject", test_stale_reject);
  run("fee_propagation", test_fee_propagation);

  if (ok) {
    std::cout << "[OK] sim_execution_assembler_test passed (5 cases: report "
                 "fields + F-12 correlation, annotation report_id correlation, "
                 "determinism, stale reject, fee propagation)."
              << std::endl;
    return 0;
  }
  return 1;
}
