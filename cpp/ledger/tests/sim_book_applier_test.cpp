// F-20 DoD-7 — SimBookApplier unit suite (pure; no PG, no Kafka).

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include "app/sim_book_applier.hpp"

namespace {

using cex::common::Decimal;
using cex::ledger::app::SimBookApplier;
using cex::ledger::app::SimBookApplyResult;
using cex::ledger::app::SimReportPair;

namespace execv1 = fob::execution::v1;
namespace commonv1 = fob::common::v1;
namespace simv1 = fob::sim::v1;

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

double D2(const Decimal& d) { return static_cast<double>(d); }

SimReportPair MakePair(const std::string& report_id, commonv1::Side side,
                       double filled, double avg_price, double reference_mid,
                       const std::string& provider = "prov-1",
                       const std::string& session = "sess-1") {
  SimReportPair p;
  p.report.set_report_id(report_id);
  p.report.set_status(execv1::EXECUTION_REPORT_STATUS_FILLED);
  p.report.set_provider_id(provider);
  p.report.set_venue("binance");
  p.report.mutable_instrument()->set_symbol("BTC/USDT");
  p.report.set_side(side);
  *p.report.mutable_filled_qty() = PD(filled);
  *p.report.mutable_average_price() = PD(avg_price);
  if (reference_mid > 0) *p.report.mutable_reference_mid() = PD(reference_mid);
  auto* fee = p.report.mutable_fee_total();
  fee->set_fee_type("taker");
  fee->mutable_cost()->set_currency("USDT");
  *fee->mutable_cost()->mutable_amount() = PD(0.5);
  p.annotation.set_report_id(report_id);
  p.annotation.set_sim_session_id(session);
  return p;
}

// 1. Happy BUY: position +qty, hedge_pnl from (ref - exec)*qty positive,
//    fee passes through, trade_count=1.
bool test_happy_buy() {
  SimBookApplier a;
  // exec 100, ref 101 -> ref - exec = +1, BUY: pnl = +1 * 0.5 = +0.5.
  auto r = a.Apply(MakePair("r1", commonv1::SIDE_BUY, 0.5, 100.0, 101.0));
  bool ok = true;
  ok &= Check(r.applied, "buy: applied");
  ok &= Check(r.position_delta.has_value(), "buy: position delta present");
  ok &= Check(r.hedge_pnl_delta.has_value(), "buy: pnl delta present");
  ok &= Check(std::abs(D2(r.position_delta->signed_qty_delta) - 0.5) < 1e-9,
              "buy: signed qty +0.5");
  ok &= Check(std::abs(D2(r.position_delta->exec_price) - 100.0) < 1e-9,
              "buy: exec price 100");
  ok &= Check(std::abs(D2(r.hedge_pnl_delta->hedge_pnl_delta) - 0.5) < 1e-9,
              "buy: hedge_pnl = (ref-exec)*qty = +0.5");
  ok &= Check(std::abs(D2(r.hedge_pnl_delta->fee_delta) - 0.5) < 1e-9,
              "buy: fee 0.5");
  ok &= Check(std::abs(D2(r.hedge_pnl_delta->filled_qty_delta) - 0.5) < 1e-9,
              "buy: filled qty 0.5");
  ok &= Check(r.hedge_pnl_delta->trade_count_delta == 1, "buy: trade_count=1");
  return ok;
}

// 2. Happy SELL: position -qty, hedge_pnl sign-flipped.
bool test_happy_sell() {
  SimBookApplier a;
  // exec 102, ref 100 -> ref - exec = -2, SELL: pnl = +2 * 1.0 = +2 (good
  // hedge: sold above reference).
  auto r = a.Apply(MakePair("r2", commonv1::SIDE_SELL, 1.0, 102.0, 100.0));
  bool ok = true;
  ok &= Check(r.applied, "sell: applied");
  ok &= Check(std::abs(D2(r.position_delta->signed_qty_delta) + 1.0) < 1e-9,
              "sell: signed qty -1.0");
  ok &= Check(std::abs(D2(r.hedge_pnl_delta->hedge_pnl_delta) - 2.0) < 1e-9,
              "sell: hedge_pnl = -(ref-exec)*qty = +2.0");
  return ok;
}

// 3. Duplicate report_id is a no-op the second time.
bool test_duplicate_skipped() {
  SimBookApplier a;
  auto r1 = a.Apply(MakePair("r3", commonv1::SIDE_BUY, 0.5, 100.0, 101.0));
  auto r2 = a.Apply(MakePair("r3", commonv1::SIDE_BUY, 0.5, 100.0, 101.0));
  return Check(r1.applied && !r2.applied && r2.skip_reason == "duplicate" &&
                   a.seen_count() == 1,
               "duplicate: second skipped, seen_count stays 1");
}

// 4. Empty sim_session_id -> skip.
bool test_no_session() {
  SimBookApplier a;
  auto p = MakePair("r4", commonv1::SIDE_BUY, 0.5, 100.0, 101.0, "prov-1", "");
  auto r = a.Apply(p);
  return Check(!r.applied && r.skip_reason == "no_session_id", "no session");
}

// 5. Non-terminal status (PENDING) -> skip.
bool test_non_terminal() {
  SimBookApplier a;
  auto p = MakePair("r5", commonv1::SIDE_BUY, 0.5, 100.0, 101.0);
  p.report.set_status(execv1::EXECUTION_REPORT_STATUS_REJECTED);
  auto r = a.Apply(p);
  return Check(!r.applied && r.skip_reason == "non_terminal", "non terminal");
}

// 6. Zero filled_qty -> skip (a FILLED-status report with 0 fill is invalid
//    but defensive).
bool test_zero_fill() {
  SimBookApplier a;
  auto p = MakePair("r6", commonv1::SIDE_BUY, 0.0, 100.0, 101.0);
  auto r = a.Apply(p);
  return Check(!r.applied && r.skip_reason == "zero_fill", "zero fill");
}

// 7. Missing reference_mid -> hedge_pnl=0; trade still recorded (fee + qty +
//    count) so totals are consistent.
bool test_no_reference_mid() {
  SimBookApplier a;
  auto r = a.Apply(MakePair("r7", commonv1::SIDE_BUY, 0.5, 100.0, 0.0));
  bool ok = true;
  ok &= Check(r.applied, "no ref: still applied");
  ok &= Check(r.hedge_pnl_delta->hedge_pnl_delta.units == 0,
              "no ref: hedge_pnl_delta=0");
  ok &= Check(std::abs(D2(r.hedge_pnl_delta->filled_qty_delta) - 0.5) < 1e-9,
              "no ref: filled qty still 0.5");
  return ok;
}

// 8. Missing provider_id -> skip (sim_positions PK needs it).
bool test_no_provider() {
  SimBookApplier a;
  auto p = MakePair("r8", commonv1::SIDE_BUY, 0.5, 100.0, 101.0, "");
  auto r = a.Apply(p);
  return Check(!r.applied && r.skip_reason == "no_provider", "no provider");
}

}  // namespace

int main() {
  bool ok = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) { std::cerr << "  in test: " << name << std::endl; ok = false; }
  };

  run("happy_buy", test_happy_buy);
  run("happy_sell", test_happy_sell);
  run("duplicate_skipped", test_duplicate_skipped);
  run("no_session", test_no_session);
  run("non_terminal", test_non_terminal);
  run("zero_fill", test_zero_fill);
  run("no_reference_mid", test_no_reference_mid);
  run("no_provider", test_no_provider);

  if (ok) {
    std::cout << "[OK] sim_book_applier_test passed (8 cases: BUY/SELL hedge "
                 "math + sign-flip, duplicate dedup, gating "
                 "session/status/qty/provider, no-ref pnl=0)."
              << std::endl;
    return 0;
  }
  return 1;
}
