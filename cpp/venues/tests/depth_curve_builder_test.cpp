#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "domain/depth_curve_builder.hpp"

namespace {

using cex::common::Decimal;
using cex::venues::domain::BookLevel;
using cex::venues::domain::BuildDepthSideCurves;
using cex::venues::domain::BuildDepthSideCurvesFromLevels;
using cex::venues::domain::CanonicalOrderBook;
using cex::venues::domain::ConvexificationConfig;
using cex::venues::domain::ExecutionSide;
using cex::venues::domain::LOfVPoint;
using cex::venues::domain::SOfQPoint;
using cex::venues::domain::ApplyConvexifiedCostLayer;
using cex::venues::domain::MonotoneInterpolationConfig;
using cex::venues::domain::BuildMonotoneLagrangianInterpolation;
using cex::venues::domain::ConvexifyCostLayer;

BookLevel Level(const int64_t price_units,
                const int64_t qty_units,
                const int32_t price_scale = 0,
                const int32_t qty_scale = 3) {
  return BookLevel{
      .price = Decimal{.units = price_units, .scale = price_scale},
      .qty = Decimal{.units = qty_units, .scale = qty_scale},
  };
}

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool CheckDecimal(const Decimal& actual,
                  const int64_t expected_units,
                  const int32_t expected_scale,
                  const std::string& field_name) {
  if (!Check(actual.units == expected_units, field_name + " units mismatch")) return false;
  if (!Check(actual.scale == expected_scale, field_name + " scale mismatch")) return false;
  return true;
}

bool CheckApprox(const double actual,
                 const double expected,
                 const double tolerance,
                 const std::string& field_name) {
  if (std::fabs(actual - expected) <= tolerance) return true;
  std::cerr << "[FAIL] " << field_name << " expected " << expected
            << " got " << actual << std::endl;
  return false;
}

double SecondDifference(const std::vector<SOfQPoint>& s_of_q, std::size_t i) {
  return static_cast<double>(s_of_q[i + 1].cumulative_cost) -
         2.0 * static_cast<double>(s_of_q[i].cumulative_cost) +
         static_cast<double>(s_of_q[i - 1].cumulative_cost);
}

bool TestBuySideL1Example() {
  // F-11 Level 1 example:
  // asks: 70_000@1, 70_010@1, 70_020@1
  // expected p(q): p(0)=70_000, p(1)=70_000, p(2)=70_010, p(3)=70_020
  const std::vector<BookLevel> asks = {
      Level(70000, 1000),
      Level(70010, 1000),
      Level(70020, 1000),
  };

  const auto curves = BuildDepthSideCurvesFromLevels(asks, ExecutionSide::kBuy);
  if (!Check(curves.side == ExecutionSide::kBuy, "side must be buy")) return false;

  if (!Check(curves.q_of_p.size() == 3, "Q(p) for buy must have 3 points")) return false;
  if (!CheckDecimal(curves.q_of_p[0].price, 70000, 0, "Q(p)[0].price")) return false;
  if (!CheckDecimal(curves.q_of_p[0].cumulative_qty, 1000, 3, "Q(p)[0].qty")) return false;
  if (!CheckDecimal(curves.q_of_p[1].price, 70010, 0, "Q(p)[1].price")) return false;
  if (!CheckDecimal(curves.q_of_p[1].cumulative_qty, 2000, 3, "Q(p)[1].qty")) return false;
  if (!CheckDecimal(curves.q_of_p[2].price, 70020, 0, "Q(p)[2].price")) return false;
  if (!CheckDecimal(curves.q_of_p[2].cumulative_qty, 3000, 3, "Q(p)[2].qty")) return false;

  if (!Check(curves.p_of_q.size() == 4, "p(q) for buy must have 4 points including q=0")) {
    return false;
  }
  if (!CheckDecimal(curves.p_of_q[0].qty, 0, 3, "p(q)[0].q")) return false;
  if (!CheckDecimal(curves.p_of_q[0].price, 70000, 0, "p(q)[0].p")) return false;
  if (!CheckDecimal(curves.p_of_q[1].qty, 1000, 3, "p(q)[1].q")) return false;
  if (!CheckDecimal(curves.p_of_q[1].price, 70000, 0, "p(q)[1].p")) return false;
  if (!CheckDecimal(curves.p_of_q[2].qty, 2000, 3, "p(q)[2].q")) return false;
  if (!CheckDecimal(curves.p_of_q[2].price, 70010, 0, "p(q)[2].p")) return false;
  if (!CheckDecimal(curves.p_of_q[3].qty, 3000, 3, "p(q)[3].q")) return false;
  if (!CheckDecimal(curves.p_of_q[3].price, 70020, 0, "p(q)[3].p")) return false;

  return true;
}

bool TestSellSideCurves() {
  // Sell executes into bids (best bid to deeper/lower bids).
  const std::vector<BookLevel> bids = {
      Level(70000, 1000),
      Level(69990, 2000),
      Level(69980, 1000),
  };

  const auto curves = BuildDepthSideCurvesFromLevels(bids, ExecutionSide::kSell);
  if (!Check(curves.side == ExecutionSide::kSell, "side must be sell")) return false;

  if (!Check(curves.q_of_p.size() == 3, "Q(p) for sell must have 3 points")) return false;
  if (!CheckDecimal(curves.q_of_p[0].cumulative_qty, 1000, 3, "sell Q(p)[0].qty")) return false;
  if (!CheckDecimal(curves.q_of_p[1].cumulative_qty, 3000, 3, "sell Q(p)[1].qty")) return false;
  if (!CheckDecimal(curves.q_of_p[2].cumulative_qty, 4000, 3, "sell Q(p)[2].qty")) return false;

  if (!Check(curves.p_of_q.size() == 4, "p(q) for sell must have 4 points")) return false;
  if (!CheckDecimal(curves.p_of_q[0].price, 70000, 0, "sell p(q)[0].p")) return false;
  if (!CheckDecimal(curves.p_of_q[1].price, 70000, 0, "sell p(q)[1].p")) return false;
  if (!CheckDecimal(curves.p_of_q[2].price, 69990, 0, "sell p(q)[2].p")) return false;
  if (!CheckDecimal(curves.p_of_q[3].price, 69980, 0, "sell p(q)[3].p")) return false;

  // Monotonicity on sell side: marginal price must be non-increasing by q.
  if (!Check(curves.p_of_q[0].price.units >= curves.p_of_q[1].price.units,
             "sell p(q) monotonicity violated at [0]->[1]")) {
    return false;
  }
  if (!Check(curves.p_of_q[1].price.units >= curves.p_of_q[2].price.units,
             "sell p(q) monotonicity violated at [1]->[2]")) {
    return false;
  }
  if (!Check(curves.p_of_q[2].price.units >= curves.p_of_q[3].price.units,
             "sell p(q) monotonicity violated at [2]->[3]")) {
    return false;
  }

  return true;
}

bool TestOrderBookSideSelection() {
  CanonicalOrderBook book;
  book.asks = {
      Level(70010, 1000),
      Level(70020, 1000),
  };
  book.bids = {
      Level(70000, 1000),
      Level(69990, 1000),
  };

  const auto buy_curves = BuildDepthSideCurves(book, ExecutionSide::kBuy);
  if (!Check(!buy_curves.empty(), "buy curves must be non-empty")) return false;
  if (!CheckDecimal(buy_curves.p_of_q[0].price, 70010, 0, "buy side must use asks")) return false;

  const auto sell_curves = BuildDepthSideCurves(book, ExecutionSide::kSell);
  if (!Check(!sell_curves.empty(), "sell curves must be non-empty")) return false;
  if (!CheckDecimal(sell_curves.p_of_q[0].price, 70000, 0, "sell side must use bids")) return false;

  return true;
}

bool TestEmptyInput() {
  const std::vector<BookLevel> empty_levels;
  const auto curves = BuildDepthSideCurvesFromLevels(empty_levels, ExecutionSide::kBuy);
  if (!Check(curves.empty(), "curves from empty levels must be empty")) return false;
  if (!Check(curves.p_of_q.empty(), "p(q) from empty levels must be empty")) return false;
  return true;
}

bool TestIntegratedCostAndLagrangian() {
  // F11-CURVE-3:
  // asks: 70_000@1, 70_010@1, 70_020@1, tau=1s
  // S(1)=70_000, S(2)=140_010, S(3)=210_030
  const std::vector<BookLevel> asks = {
      Level(70000, 1000),
      Level(70010, 1000),
      Level(70020, 1000),
  };
  const Decimal tau_sec{.units = 1, .scale = 0};
  const auto curves = BuildDepthSideCurvesFromLevels(asks, ExecutionSide::kBuy, tau_sec);

  if (!Check(curves.s_of_q.size() == 4, "S(q) must contain q=0 and 3 cumulative points")) {
    return false;
  }
  if (!CheckDecimal(curves.s_of_q[0].qty, 0, 3, "S(q)[0].q")) return false;
  if (!CheckDecimal(curves.s_of_q[0].cumulative_cost, 0, 3, "S(q)[0].S")) return false;
  if (!CheckDecimal(curves.s_of_q[1].cumulative_cost, 70000000, 3, "S(q)[1].S")) return false;
  if (!CheckDecimal(curves.s_of_q[2].cumulative_cost, 140010000, 3, "S(q)[2].S")) return false;
  if (!CheckDecimal(curves.s_of_q[3].cumulative_cost, 210030000, 3, "S(q)[3].S")) return false;

  if (!Check(curves.l_of_v.size() == 4, "L(v) must match S(q) grid size")) return false;
  if (!CheckApprox(static_cast<double>(curves.l_of_v[0].speed), 0.0, 1e-9, "v[0]")) return false;
  if (!CheckApprox(static_cast<double>(curves.l_of_v[1].speed), 1.0, 1e-9, "v[1]")) return false;
  if (!CheckApprox(static_cast<double>(curves.l_of_v[2].speed), 2.0, 1e-9, "v[2]")) return false;
  if (!CheckApprox(static_cast<double>(curves.l_of_v[3].speed), 3.0, 1e-9, "v[3]")) return false;

  if (!CheckApprox(static_cast<double>(curves.l_of_v[1].lagrangian), 70000.0, 1e-6, "L(v1)")) {
    return false;
  }
  if (!CheckApprox(static_cast<double>(curves.l_of_v[2].lagrangian), 140010.0, 1e-6, "L(v2)")) {
    return false;
  }
  if (!CheckApprox(static_cast<double>(curves.l_of_v[3].lagrangian), 210030.0, 1e-6, "L(v3)")) {
    return false;
  }
  return true;
}

bool TestTauScaling() {
  const std::vector<BookLevel> asks = {
      Level(70000, 1000),
      Level(70010, 1000),
      Level(70020, 1000),
  };
  const Decimal tau_sec{.units = 2, .scale = 0};
  const auto curves = BuildDepthSideCurvesFromLevels(asks, ExecutionSide::kBuy, tau_sec);

  if (!Check(curves.l_of_v.size() == 4, "L(v) with tau=2 must have 4 points")) return false;
  if (!CheckApprox(static_cast<double>(curves.l_of_v[1].speed), 0.5, 1e-9, "tau2 v[1]")) {
    return false;
  }
  if (!CheckApprox(static_cast<double>(curves.l_of_v[2].speed), 1.0, 1e-9, "tau2 v[2]")) {
    return false;
  }
  if (!CheckApprox(static_cast<double>(curves.l_of_v[3].speed), 1.5, 1e-9, "tau2 v[3]")) {
    return false;
  }

  // L(v) = S(v*tau)/tau => half of tau=1 values when tau=2.
  if (!CheckApprox(static_cast<double>(curves.l_of_v[1].lagrangian), 35000.0, 1e-6,
                   "tau2 L(v1)")) {
    return false;
  }
  if (!CheckApprox(static_cast<double>(curves.l_of_v[2].lagrangian), 70005.0, 1e-6,
                   "tau2 L(v2)")) {
    return false;
  }
  if (!CheckApprox(static_cast<double>(curves.l_of_v[3].lagrangian), 105015.0, 1e-6,
                   "tau2 L(v3)")) {
    return false;
  }
  return true;
}

bool TestMonotonicApproximationForGlitchyInput() {
  // F11 docs require monotonic L1 p(q) approximation.
  // Input contains a downward glitch at level 3.
  const std::vector<BookLevel> asks = {
      Level(70000, 1000),
      Level(70005, 1000),
      Level(69990, 1000),  // glitch
      Level(70020, 1000),
  };
  const auto curves = BuildDepthSideCurvesFromLevels(asks, ExecutionSide::kBuy);

  if (!Check(curves.p_of_q.size() == 5, "glitchy input p(q) must keep 5 points")) return false;
  for (std::size_t i = 1; i < curves.p_of_q.size(); ++i) {
    if (!Check(curves.p_of_q[i].price.units >= curves.p_of_q[i - 1].price.units,
               "buy p(q) must remain non-decreasing after monotonic approximation")) {
      return false;
    }
  }

  if (!CheckDecimal(curves.p_of_q[2].price, 70005, 0, "glitchy p(q)[2]")) return false;
  if (!CheckDecimal(curves.p_of_q[3].price, 70005, 0, "glitchy p(q)[3] flattened")) return false;
  return true;
}

bool TestHighScaleCostLayerDoesNotOverflow() {
  // Live BTC/USDT CEX feeds commonly arrive with scale=8 for both price and qty.
  // The integrated cost layer S(q) must stay numerically stable on such inputs.
  const std::vector<BookLevel> bids = {
      Level(7768318000000LL, 435964000LL, 8, 8),
      Level(7768317000000LL, 238000LL, 8, 8),
      Level(7768228000000LL, 120000LL, 8, 8),
      Level(7768227000000LL, 1126000LL, 8, 8),
  };

  const auto curves = BuildDepthSideCurvesFromLevels(bids, ExecutionSide::kSell);
  if (!Check(curves.s_of_q.size() == 5, "high-scale S(q) must contain q=0 + 4 points")) {
    return false;
  }

  for (std::size_t i = 1; i < curves.s_of_q.size(); ++i) {
    if (!Check(static_cast<double>(curves.s_of_q[i].cumulative_cost) >
                   static_cast<double>(curves.s_of_q[i - 1].cumulative_cost),
               "high-scale S(q) must remain strictly increasing")) {
      return false;
    }
  }

  bool pass = true;
  pass = CheckApprox(static_cast<double>(curves.s_of_q[1].cumulative_cost),
                     338670.6988552,
                     1e-6,
                     "high-scale S(q)[1]") && pass;
  pass = CheckApprox(static_cast<double>(curves.s_of_q.back().cumulative_cost),
                     339823.505896,
                     1e-6,
                     "high-scale S(q)[last]") && pass;
  pass = Check(curves.p_of_q[0].price.units >= curves.p_of_q[1].price.units,
               "high-scale sell p(q) must stay monotone") && pass;
  return pass;
}

bool TestMonotoneInterpolationOnLagrangian() {
  const std::vector<BookLevel> asks = {
      Level(70000, 1000),
      Level(70005, 1000),
      Level(70020, 1000),
      Level(70025, 1000),
  };

  const auto curves = BuildDepthSideCurvesFromLevels(asks, ExecutionSide::kBuy);
  if (!Check(curves.l_of_v_monotone.size() > curves.l_of_v.size(),
             "monotone interpolation must densify L(v)")) {
    return false;
  }

  if (!CheckDecimal(curves.l_of_v_monotone.front().speed,
                    curves.l_of_v.front().speed.units,
                    curves.l_of_v.front().speed.scale,
                    "L_interp first speed")) {
    return false;
  }
  if (!CheckDecimal(curves.l_of_v_monotone.front().lagrangian,
                    curves.l_of_v.front().lagrangian.units,
                    curves.l_of_v.front().lagrangian.scale,
                    "L_interp first lagrangian")) {
    return false;
  }
  if (!CheckDecimal(curves.l_of_v_monotone.back().speed,
                    curves.l_of_v.back().speed.units,
                    curves.l_of_v.back().speed.scale,
                    "L_interp last speed")) {
    return false;
  }
  if (!CheckDecimal(curves.l_of_v_monotone.back().lagrangian,
                    curves.l_of_v.back().lagrangian.units,
                    curves.l_of_v.back().lagrangian.scale,
                    "L_interp last lagrangian")) {
    return false;
  }

  for (std::size_t i = 1; i < curves.l_of_v_monotone.size(); ++i) {
    if (!Check(curves.l_of_v_monotone[i].speed.units >=
               curves.l_of_v_monotone[i - 1].speed.units,
               "L_interp speed grid must be non-decreasing")) {
      return false;
    }
    if (!Check(curves.l_of_v_monotone[i].lagrangian.units >=
               curves.l_of_v_monotone[i - 1].lagrangian.units,
               "L_interp lagrangian must be non-decreasing")) {
      return false;
    }
  }

  return true;
}

bool TestShapePreservingNoOvershoot() {
  // Explicit monotone knots with uneven slopes: interpolation must not create
  // oscillations or go below/above segment endpoints.
  std::vector<LOfVPoint> knots = {
      {.speed = Decimal{.units = 0, .scale = 0},
       .lagrangian = Decimal{.units = 0, .scale = 0}},
      {.speed = Decimal{.units = 1, .scale = 0},
       .lagrangian = Decimal{.units = 1000, .scale = 0}},
      {.speed = Decimal{.units = 2, .scale = 0},
       .lagrangian = Decimal{.units = 1100, .scale = 0}},
      {.speed = Decimal{.units = 3, .scale = 0},
       .lagrangian = Decimal{.units = 3000, .scale = 0}},
  };

  const auto interp = BuildMonotoneLagrangianInterpolation(
      knots, MonotoneInterpolationConfig{.subdivisions_per_segment = 16});

  if (!Check(interp.size() > knots.size(), "explicit interpolation must add points")) {
    return false;
  }

  for (std::size_t i = 1; i < interp.size(); ++i) {
    if (!Check(interp[i].lagrangian.units >= interp[i - 1].lagrangian.units,
               "shape-preserving interpolation must stay monotone")) {
      return false;
    }
  }

  if (!CheckApprox(static_cast<double>(interp.front().lagrangian),
                   static_cast<double>(knots.front().lagrangian),
                   1e-9,
                   "shape-preserving first knot mismatch")) {
    return false;
  }
  if (!CheckApprox(static_cast<double>(interp.back().lagrangian),
                   static_cast<double>(knots.back().lagrangian),
                   1e-6,
                   "shape-preserving last knot mismatch")) {
    return false;
  }

  return true;
}

bool TestConvexifyCostLayerMakesSConvex() {
  std::vector<SOfQPoint> s_of_q = {
      {.qty = Decimal{.units = 0, .scale = 0},
       .cumulative_cost = Decimal{.units = 0, .scale = 0}},
      {.qty = Decimal{.units = 1, .scale = 0},
       .cumulative_cost = Decimal{.units = 10, .scale = 0}},
      {.qty = Decimal{.units = 2, .scale = 0},
       .cumulative_cost = Decimal{.units = 19, .scale = 0}},
      {.qty = Decimal{.units = 3, .scale = 0},
       .cumulative_cost = Decimal{.units = 27, .scale = 0}},
      {.qty = Decimal{.units = 4, .scale = 0},
       .cumulative_cost = Decimal{.units = 40, .scale = 0}},
  };

  const auto convex = ConvexifyCostLayer(s_of_q);
  if (!Check(convex.size() == s_of_q.size(), "convexified S(q) size mismatch")) return false;

  for (std::size_t i = 1; i + 1 < convex.size(); ++i) {
    if (!Check(SecondDifference(convex, i) >= -1e-8,
               "convexified S(q) must have non-negative second differences")) {
      return false;
    }
  }
  return true;
}

bool TestConvexifyCostLayerRespectsRange() {
  std::vector<SOfQPoint> s_of_q = {
      {.qty = Decimal{.units = 0, .scale = 0},
       .cumulative_cost = Decimal{.units = 0, .scale = 0}},
      {.qty = Decimal{.units = 1, .scale = 0},
       .cumulative_cost = Decimal{.units = 10, .scale = 0}},
      {.qty = Decimal{.units = 2, .scale = 0},
       .cumulative_cost = Decimal{.units = 20, .scale = 0}},
      {.qty = Decimal{.units = 3, .scale = 0},
       .cumulative_cost = Decimal{.units = 27, .scale = 0}},
      {.qty = Decimal{.units = 4, .scale = 0},
       .cumulative_cost = Decimal{.units = 33, .scale = 0}},
      {.qty = Decimal{.units = 5, .scale = 0},
       .cumulative_cost = Decimal{.units = 40, .scale = 0}},
  };

  ConvexificationConfig cfg;
  cfg.q_min = Decimal{.units = 2, .scale = 0};
  cfg.q_max = Decimal{.units = 4, .scale = 0};

  const auto convex = ConvexifyCostLayer(s_of_q, cfg);
  if (!Check(convex.size() == s_of_q.size(), "range convexified S(q) size mismatch")) {
    return false;
  }

  if (!CheckDecimal(convex[0].cumulative_cost, s_of_q[0].cumulative_cost.units,
                    s_of_q[0].cumulative_cost.scale, "range convex S[0]")) {
    return false;
  }
  if (!CheckDecimal(convex[1].cumulative_cost, s_of_q[1].cumulative_cost.units,
                    s_of_q[1].cumulative_cost.scale, "range convex S[1]")) {
    return false;
  }
  if (!CheckDecimal(convex[5].cumulative_cost, s_of_q[5].cumulative_cost.units,
                    s_of_q[5].cumulative_cost.scale, "range convex S[5]")) {
    return false;
  }

  if (!Check(SecondDifference(convex, 3) >= -1e-8,
             "range convex S(q) must be convex in selected interval")) {
    return false;
  }
  return true;
}

bool TestApplyConvexifiedCostLayerRebuildsCurves() {
  const std::vector<BookLevel> asks = {
      Level(70000, 1000),
      Level(70005, 1000),
      Level(70010, 1000),
      Level(70020, 1000),
  };

  auto curves = BuildDepthSideCurvesFromLevels(asks, ExecutionSide::kBuy);
  curves.s_of_q = {
      {.qty = Decimal{.units = 0, .scale = 0},
       .cumulative_cost = Decimal{.units = 0, .scale = 0}},
      {.qty = Decimal{.units = 1, .scale = 0},
       .cumulative_cost = Decimal{.units = 10, .scale = 0}},
      {.qty = Decimal{.units = 2, .scale = 0},
       .cumulative_cost = Decimal{.units = 19, .scale = 0}},
      {.qty = Decimal{.units = 3, .scale = 0},
       .cumulative_cost = Decimal{.units = 27, .scale = 0}},
      {.qty = Decimal{.units = 4, .scale = 0},
       .cumulative_cost = Decimal{.units = 40, .scale = 0}},
  };

  const auto rebuilt = ApplyConvexifiedCostLayer(curves);
  if (!Check(rebuilt.s_of_q.size() == curves.s_of_q.size(), "rebuilt S(q) size mismatch")) {
    return false;
  }
  if (!Check(rebuilt.p_of_q.size() == rebuilt.s_of_q.size(), "rebuilt p(q) size mismatch")) {
    return false;
  }
  if (!Check(rebuilt.q_of_p.size() + 1 == rebuilt.s_of_q.size(),
             "rebuilt Q(p) must be one point shorter than S(q)")) {
    return false;
  }
  if (!Check(rebuilt.l_of_v.size() == rebuilt.s_of_q.size(), "rebuilt L(v) size mismatch")) {
    return false;
  }
  if (!Check(!rebuilt.l_of_v_monotone.empty(), "rebuilt monotone L(v) must not be empty")) {
    return false;
  }

  for (std::size_t i = 1; i < rebuilt.p_of_q.size(); ++i) {
    if (!Check(rebuilt.p_of_q[i].price.units >= rebuilt.p_of_q[i - 1].price.units,
               "rebuilt p(q) must remain non-decreasing for buy side")) {
      return false;
    }
  }
  for (std::size_t i = 1; i + 1 < rebuilt.s_of_q.size(); ++i) {
    if (!Check(SecondDifference(rebuilt.s_of_q, i) >= -1e-8,
               "rebuilt S(q) must be convex")) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  if (!TestBuySideL1Example()) return EXIT_FAILURE;
  if (!TestSellSideCurves()) return EXIT_FAILURE;
  if (!TestOrderBookSideSelection()) return EXIT_FAILURE;
  if (!TestEmptyInput()) return EXIT_FAILURE;
  if (!TestIntegratedCostAndLagrangian()) return EXIT_FAILURE;
  if (!TestTauScaling()) return EXIT_FAILURE;
  if (!TestMonotonicApproximationForGlitchyInput()) return EXIT_FAILURE;
  if (!TestHighScaleCostLayerDoesNotOverflow()) return EXIT_FAILURE;
  if (!TestMonotoneInterpolationOnLagrangian()) return EXIT_FAILURE;
  if (!TestShapePreservingNoOvershoot()) return EXIT_FAILURE;
  if (!TestConvexifyCostLayerMakesSConvex()) return EXIT_FAILURE;
  if (!TestConvexifyCostLayerRespectsRange()) return EXIT_FAILURE;
  if (!TestApplyConvexifiedCostLayerRebuildsCurves()) return EXIT_FAILURE;

  std::cout << "[OK] depth_curve_builder_test passed" << std::endl;
  return EXIT_SUCCESS;
}
