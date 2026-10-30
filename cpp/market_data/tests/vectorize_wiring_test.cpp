// ============================================================================
// vectorize_wiring_test.cpp — F-05A (T-F05A-205). Hand-rolled harness.
//
// curve_to_levels → Vectorize → ToVectorizedSnapshot (proto). Проверяет:
//   1) реконструкцию уровней из VenueLiquidityCurve (bid+ask, marginal ladder);
//   2) end-to-end маппинг в proto VectorizedLiquiditySnapshot (basis, сегменты,
//      w как Decimal, side enum, q_max).
// ============================================================================

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "app/curve_to_levels.hpp"
#include "domain/vectorize.hpp"
#include "transport/mappers/vectorized_liquidity.hpp"

namespace {

namespace app = cex::market_data::app;
namespace dom = cex::market_data::domain;
namespace tr = cex::market_data::transport;
namespace vv1 = fob::venue::v1;
namespace mv1 = fob::marketdata::v1;

int g_failures = 0;
bool expect(bool cond, const std::string& msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; ++g_failures; }
  return cond;
}

vv1::VenueLiquidityCurve MakeCurve() {
  vv1::VenueLiquidityCurve c;
  c.set_venue_id("vx");
  auto* inst = c.mutable_instrument();
  inst->set_symbol("BTC/USDT");
  inst->set_base("BTC");
  inst->set_quote("USDT");
  // bid: q_grid=[1,3], p_of_q=[100,99] → уровни qty=1@100, qty=2@99
  auto* bid = c.mutable_bid_curve();
  bid->add_q_grid(1.0); bid->add_q_grid(3.0);
  bid->add_p_of_q(100.0); bid->add_p_of_q(99.0);
  // ask: q_grid=[2], p_of_q=[101] → уровень qty=2@101
  auto* ask = c.mutable_ask_curve();
  ask->add_q_grid(2.0);
  ask->add_p_of_q(101.0);
  return c;
}

void TestCurveToLevels() {
  auto levels = app::LevelsFromCurve(MakeCurve());
  expect(levels.size() == 3, "curve: 3 levels (2 bid + 1 ask)");
  // первый bid: qty=1, price=100
  expect(levels[0].side == dom::LevelSide::kBid, "curve: level0 bid");
  expect(static_cast<double>(levels[0].quantity) == 1.0, "curve: level0 qty=1 (step)");
  expect(static_cast<double>(levels[0].price) == 100.0, "curve: level0 price=100");
  // второй bid: qty=2 (3-1), price=99
  expect(static_cast<double>(levels[1].quantity) == 2.0, "curve: level1 qty=2 (step)");
  // ask
  expect(levels[2].side == dom::LevelSide::kAsk && levels[2].base_asset == "BTC",
         "curve: ask level, base=BTC");
}

void TestEndToEndProto() {
  auto levels = app::LevelsFromCurve(MakeCurve());
  dom::VectorizeResult r = dom::Vectorize(levels);
  mv1::VectorizedLiquiditySnapshot snap = tr::ToVectorizedSnapshot(r, "batch-1");

  expect(snap.batch_id() == "batch-1" && snap.input().batch_id() == "batch-1",
         "proto: batch_id set");
  const auto& basis = snap.input().basis();
  expect(basis.num_assets() == 2, "proto: 2 assets in basis");
  expect(basis.assets_size() == 2 && basis.assets(0).asset() == "BTC" &&
             basis.assets(1).asset() == "USDT",
         "proto: sorted basis entries BTC/USDT");
  expect(snap.input().segments_size() == 3, "proto: 3 segments");

  // seg0 = bid BTC/USDT p=100: w=[+1(BTC), -100(USDT)]
  const auto& s0 = snap.input().segments(0);
  expect(s0.side() == mv1::VECTOR_LEVEL_SIDE_BID, "proto: seg0 side BID");
  expect(s0.w_size() == 2, "proto: w length = num_assets");
  expect(s0.w(0).units() == 1000000000000LL, "proto: w[BTC] = +1 @scale12");
  expect(s0.w(1).units() == -100000000000000LL, "proto: w[USDT] = -100 @scale12");
  expect(s0.q_max().units() == 1000000000000LL && s0.q_max().scale() == 12,
         "proto: seg0 q_max = 1.0 @scale12");
}

}  // namespace

// ADR-051: одна сторона кривой → ОДИН линейный сегмент (наклон из кривой).
void TestLinearSegments() {
  auto levels = app::LinearSegmentsFromCurve(MakeCurve());
  expect(levels.size() == 2, "linear: 2 сегмента (1 bid + 1 ask)");
  // bid: anchor=best=100, q_max=q_grid[last]=3, d_hl=|99-100|=1 (наклон).
  const auto& bid = levels[0];
  expect(bid.side == dom::LevelSide::kBid, "linear: сегм0 bid");
  expect(static_cast<double>(bid.price) == 100.0, "linear: anchor=best price 100");
  expect(static_cast<double>(bid.quantity) == 3.0, "linear: q_max=глубина 3");
  expect(static_cast<double>(bid.d_hl_override) == 1.0, "linear: d_hl=|99-100|=1 (наклон)");
  // ask: одна точка → best==worst → d_hl_override=0 (fallback на policy).
  const auto& ask = levels[1];
  expect(ask.side == dom::LevelSide::kAsk, "linear: сегм1 ask");
  expect(static_cast<double>(ask.price) == 101.0, "linear: ask anchor=101");
  expect(static_cast<double>(ask.d_hl_override) == 0.0, "linear: 1 точка → d_hl_override=0");
}

// ADR-053: двусторонний сегмент с safe-translator наклоном (VWAP-глубина + θ).
void TestSafeTranslatorSegment() {
  setenv("F05A_TRANSLATOR_MODEL", "safe_vwap", 1);
  setenv("F05A_SAFE_SHARE", "0.60", 1);
  auto vr = app::TwoSidedSegmentsFromCurves({MakeCurve()}, 12);
  expect(vr.segments.size() == 1, "safe: 1 двусторонний сегмент");
  if (vr.segments.empty()) return;
  const auto& s = vr.segments[0];
  auto approx = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
  const double mid = 100.5;
  // Тончайшая ликвидность = bid L0: D=100, δ=1e4·|ln(100/mid)| ⇒ α_ext≈2.005.
  expect(approx(static_cast<double>(s.alpha_ext), 2.00501, 5e-3), "safe: α_ext≈2.005 (bid L0)");
  expect(approx(static_cast<double>(s.theta), 0.60, 1e-6), "safe: θ=0.60");
  const double alpha_t = 0.60 * 2.00501;
  expect(approx(static_cast<double>(s.alpha_t), alpha_t, 5e-3), "safe: α_T=θ·α_ext");
  expect(approx(static_cast<double>(s.slope), mid / (1e4 * alpha_t), 5e-5), "safe: m=mid/(1e4·α_T)");
  expect(approx(static_cast<double>(s.anchor), std::log(mid), 1e-4), "safe: anchor=log(mid)");
  // Без raw_snapshots (nullptr) → источник FOB-кривая → тег safe_vwap_fob.
  expect(s.translator_model == "safe_vwap_fob", "safe: model tag (fob fallback)");
  // β_T = mid·m (линейный и log-наклон совпадают у якоря).
  expect(approx(static_cast<double>(s.beta_t), mid * static_cast<double>(s.slope), 1e-3),
         "safe: β_T=mid·m");
}

int main() {
  TestCurveToLevels();
  TestEndToEndProto();
  TestLinearSegments();
  TestSafeTranslatorSegment();
  if (g_failures == 0) { std::cout << "vectorize_wiring_test: ALL PASSED\n"; return 0; }
  std::cerr << "vectorize_wiring_test: " << g_failures << " FAILURE(S)\n";
  return 1;
}
