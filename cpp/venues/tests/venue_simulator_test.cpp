// F-20 VenueSimulator core unit suite (T-F20-701 subset, U1..U10).
//
// Deterministic — the engine samples latency/rejection from a seeded RNG,
// so given a fixed rng_seed every result is reproducible.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include "app/venue_simulator.hpp"

namespace {

using cex::common::Decimal;
using cex::venues::app::SimModels;
using cex::venues::app::SimulateRequest;
using cex::venues::app::SimulateResult;
using cex::venues::app::VenueSimulator;

bool Check(bool cond, const std::string& msg) {
  if (cond) return true;
  std::cerr << "[FAIL] " << msg << std::endl;
  return false;
}

fob::common::v1::Decimal PD(double v, int32_t scale = 8) {
  fob::common::v1::Decimal d;
  double s = v;
  for (int i = 0; i < scale; ++i) s *= 10.0;
  d.set_units(static_cast<int64_t>(std::llround(s)));
  d.set_scale(scale);
  return d;
}

double D2(const Decimal& d) { return static_cast<double>(d); }

// Build a snapshot with symmetric-ish book. asks ascending, bids descending.
fob::venue::v1::VenueSnapshot MakeSnapshot() {
  fob::venue::v1::VenueSnapshot s;
  s.mutable_meta()->set_event_id("snap-1");
  s.set_venue_id("binance");
  s.mutable_instrument()->set_symbol("BTC/USDT");
  *s.mutable_best_bid() = PD(99.0);
  *s.mutable_best_ask() = PD(101.0);
  *s.mutable_mid_price() = PD(100.0);
  // asks: 101 x1.0, 102 x2.0, 103 x3.0
  *s.add_ask_prices() = PD(101.0); *s.add_ask_quantities() = PD(1.0);
  *s.add_ask_prices() = PD(102.0); *s.add_ask_quantities() = PD(2.0);
  *s.add_ask_prices() = PD(103.0); *s.add_ask_quantities() = PD(3.0);
  // bids: 99 x1.0, 98 x2.0, 97 x3.0
  *s.add_bid_prices() = PD(99.0); *s.add_bid_quantities() = PD(1.0);
  *s.add_bid_prices() = PD(98.0); *s.add_bid_quantities() = PD(2.0);
  *s.add_bid_prices() = PD(97.0); *s.add_bid_quantities() = PD(3.0);
  return s;
}

SimModels NoOpModels() {
  SimModels m;
  m.latency.set_distribution(fob::sim::v1::LATENCY_DISTRIBUTION_FIXED);
  m.latency.set_p50_ms(10);
  m.latency.set_timeout_ms(0);   // disabled
  m.impact.set_model_type(fob::sim::v1::IMPACT_MODEL_TYPE_LEVEL_BY_LEVEL);
  m.fee.set_taker_bps(0);
  m.fee.set_maker_bps(0);
  // rejection all-off
  return m;
}

SimulateRequest BaseReq(const fob::venue::v1::VenueSnapshot& snap,
                        fob::common::v1::Side side, double qty) {
  SimulateRequest r;
  r.venue_id = "binance";
  r.symbol = "BTC/USDT";
  r.side = side;
  r.order_type = fob::execution::v1::EXEC_STRATEGY_MARKET;
  r.target_qty = Decimal{static_cast<int64_t>(std::llround(qty * 1e8)), 8};
  r.snapshot = &snap;
  r.lob_age_ms = 100;
  r.stale_threshold_ms = 2000;
  r.partial_fill_mode = fob::sim::v1::PARTIAL_FILL_MODE_LEVEL_BY_LEVEL;
  r.rng_seed = 42;
  return r;
}

// U1 — happy path FILLED. BUY 0.5 against ask 101 x1.0 -> all at 101.
bool test_u1_filled() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  auto r = sim.Simulate(BaseReq(snap, fob::common::v1::SIDE_BUY, 0.5), NoOpModels());
  bool ok = true;
  ok = Check(r.status == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED, "U1 FILLED") && ok;
  ok = Check(std::fabs(D2(r.filled_qty) - 0.5) < 1e-6, "U1 filled 0.5") && ok;
  ok = Check(std::fabs(D2(r.avg_price) - 101.0) < 1e-6, "U1 avg 101") && ok;
  return ok;
}

// U2 — partial fill. BUY 10 but only 1+2+3=6 available -> PARTIALLY_FILLED.
bool test_u2_partial() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  auto r = sim.Simulate(BaseReq(snap, fob::common::v1::SIDE_BUY, 10.0), NoOpModels());
  bool ok = true;
  ok = Check(r.status == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED, "U2 PARTIAL") && ok;
  ok = Check(std::fabs(D2(r.filled_qty) - 6.0) < 1e-6, "U2 filled 6") && ok;
  ok = Check(std::fabs(D2(r.remaining_qty) - 4.0) < 1e-6, "U2 remaining 4") && ok;
  return ok;
}

// U3 — stale LOB rejected.
bool test_u3_stale() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  auto req = BaseReq(snap, fob::common::v1::SIDE_BUY, 0.5);
  req.lob_age_ms = 5000;  // > 2000 threshold
  auto r = sim.Simulate(req, NoOpModels());
  bool ok = true;
  ok = Check(r.status == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED, "U3 REJECTED") && ok;
  ok = Check(r.reject_reason == "SIM_STALE_LOB", "U3 SIM_STALE_LOB") && ok;
  return ok;
}

// U4 — LEVEL_BY_LEVEL VWAP precision. BUY 2.0 -> 1.0@101 + 1.0@102 -> VWAP 101.5.
bool test_u4_vwap_precision() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  auto r = sim.Simulate(BaseReq(snap, fob::common::v1::SIDE_BUY, 2.0), NoOpModels());
  bool ok = true;
  ok = Check(std::fabs(D2(r.filled_qty) - 2.0) < 1e-6, "U4 filled 2") && ok;
  ok = Check(std::fabs(D2(r.avg_price) - 101.5) < 1e-6, "U4 VWAP 101.5") && ok;
  // impact_bps vs mid 100: (101.5-100)/100*1e4 = 150 bps
  ok = Check(std::fabs(r.impact_bps - 150.0) < 0.5, "U4 impact ~150bps") && ok;
  return ok;
}

// U5 — ImpactModel LINEAR adds delta on top of VWAP.
bool test_u5_impact_linear() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  auto m = NoOpModels();
  m.impact.set_model_type(fob::sim::v1::IMPACT_MODEL_TYPE_LINEAR);
  m.impact.set_impact_coeff(1.0);   // delta = 1.0 * filled
  // BUY 1.0 @101 (VWAP 101), LINEAR delta = 1.0*1.0 = 1.0 -> avg 102.
  auto r = sim.Simulate(BaseReq(snap, fob::common::v1::SIDE_BUY, 1.0), m);
  bool ok = true;
  ok = Check(std::fabs(D2(r.avg_price) - 102.0) < 1e-6, "U5 LINEAR avg 102") && ok;
  return ok;
}

// U6 — FeeModel taker fee applied. BUY 1.0 @101, taker 10bps -> fee 101*0.001=0.101.
bool test_u6_fee_taker() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  auto m = NoOpModels();
  m.fee.set_taker_bps(10);
  auto req = BaseReq(snap, fob::common::v1::SIDE_BUY, 1.0);
  req.order_type = fob::execution::v1::EXEC_STRATEGY_MARKET;  // taker
  auto r = sim.Simulate(req, m);
  bool ok = true;
  ok = Check(std::fabs(D2(r.fee) - 0.101) < 1e-4, "U6 taker fee 0.101") && ok;
  return ok;
}

// U7 — RejectionModel: random reject with rate 1.0 always rejects.
bool test_u7_random_reject() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  auto m = NoOpModels();
  m.rejection.set_random_rejection_rate(1.0);
  auto r = sim.Simulate(BaseReq(snap, fob::common::v1::SIDE_BUY, 0.5), m);
  bool ok = true;
  ok = Check(r.status == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED, "U7 REJECTED") && ok;
  ok = Check(r.reject_reason == "SIM_RANDOM_REJECT", "U7 SIM_RANDOM_REJECT") && ok;
  return ok;
}

// U8 — LatencyModel timeout -> SIM_TIMEOUT. FIXED 500ms > timeout 100ms.
bool test_u8_latency_timeout() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  auto m = NoOpModels();
  m.latency.set_distribution(fob::sim::v1::LATENCY_DISTRIBUTION_FIXED);
  m.latency.set_p50_ms(500);
  m.latency.set_timeout_ms(100);
  auto r = sim.Simulate(BaseReq(snap, fob::common::v1::SIDE_BUY, 0.5), m);
  bool ok = true;
  ok = Check(r.status == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED, "U8 REJECTED") && ok;
  ok = Check(r.reject_reason == "SIM_TIMEOUT", "U8 SIM_TIMEOUT") && ok;
  ok = Check(r.latency_sample_ms == 500, "U8 latency sample recorded") && ok;
  return ok;
}

// U8b — LatencyModel FIXED under timeout -> fills, latency recorded.
bool test_u8b_latency_ok() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  auto m = NoOpModels();
  m.latency.set_distribution(fob::sim::v1::LATENCY_DISTRIBUTION_FIXED);
  m.latency.set_p50_ms(35);
  m.latency.set_timeout_ms(1000);
  auto r = sim.Simulate(BaseReq(snap, fob::common::v1::SIDE_BUY, 0.5), m);
  bool ok = true;
  ok = Check(r.status == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED, "U8b FILLED") && ok;
  ok = Check(r.latency_sample_ms == 35, "U8b latency 35ms") && ok;
  return ok;
}

// U10 — overfill guard: never fills more than target even with deep book.
bool test_u10_no_overfill() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  // target 0.5, book has 6.0 -> must fill exactly 0.5.
  auto r = sim.Simulate(BaseReq(snap, fob::common::v1::SIDE_BUY, 0.5), NoOpModels());
  bool ok = true;
  ok = Check(D2(r.filled_qty) <= 0.5 + 1e-9, "U10 no overfill (<= target)") && ok;
  ok = Check(std::fabs(D2(r.filled_qty) - 0.5) < 1e-6, "U10 exact target") && ok;
  return ok;
}

// SELL side sanity: BUY hits asks, SELL hits bids.
bool test_sell_hits_bids() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  // SELL 1.0 hits best bid 99.
  auto r = sim.Simulate(BaseReq(snap, fob::common::v1::SIDE_SELL, 1.0), NoOpModels());
  bool ok = true;
  ok = Check(std::fabs(D2(r.avg_price) - 99.0) < 1e-6, "SELL avg 99 (bid side)") && ok;
  return ok;
}

// Determinism: same seed -> identical result for stochastic latency.
bool test_determinism() {
  auto snap = MakeSnapshot();
  VenueSimulator sim;
  auto m = NoOpModels();
  m.latency.set_distribution(fob::sim::v1::LATENCY_DISTRIBUTION_LOGNORMAL);
  m.latency.set_p50_ms(35);
  m.latency.set_p95_ms(90);
  auto req = BaseReq(snap, fob::common::v1::SIDE_BUY, 0.5);
  req.rng_seed = 12345;
  auto r1 = sim.Simulate(req, m);
  auto r2 = sim.Simulate(req, m);
  return Check(r1.latency_sample_ms == r2.latency_sample_ms,
               "determinism: same seed -> same latency");
}

}  // namespace

int main() {
  bool ok = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) { std::cerr << "  in test: " << name << std::endl; ok = false; }
  };

  run("U1_filled", test_u1_filled);
  run("U2_partial", test_u2_partial);
  run("U3_stale", test_u3_stale);
  run("U4_vwap_precision", test_u4_vwap_precision);
  run("U5_impact_linear", test_u5_impact_linear);
  run("U6_fee_taker", test_u6_fee_taker);
  run("U7_random_reject", test_u7_random_reject);
  run("U8_latency_timeout", test_u8_latency_timeout);
  run("U8b_latency_ok", test_u8b_latency_ok);
  run("U10_no_overfill", test_u10_no_overfill);
  run("sell_hits_bids", test_sell_hits_bids);
  run("determinism", test_determinism);

  if (ok) {
    std::cout << "[OK] venue_simulator_test passed (12 cases; U1-U10 core + "
                 "sell-side + determinism). U9 multi-venue split -> matching "
                 "execution_planner_test; SHADOW fork -> VenueSimRouter (Phase 4)."
              << std::endl;
    return 0;
  }
  return 1;
}
