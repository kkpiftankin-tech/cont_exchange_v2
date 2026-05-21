#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/depth_canonicalizer.hpp"

namespace cex::venues::domain {

enum class ExecutionSide {
  kBuy,
  kSell,
};

// Tabular representation of cumulative depth function Q(p):
// at each price point it stores cumulative executable quantity.
struct QOfPPoint {
  cex::common::Decimal price;
  cex::common::Decimal cumulative_qty;
};

// Tabular representation of marginal price function p(q):
// piecewise-constant marginal price over cumulative quantity.
struct POfQPoint {
  cex::common::Decimal qty;
  cex::common::Decimal price;
};

// Tabular representation of integrated execution cost S(q).
struct SOfQPoint {
  cex::common::Decimal qty;
  cex::common::Decimal cumulative_cost;
};

// Tabular representation of FOB Lagrangian L(v) over speed grid v=q/tau.
struct LOfVPoint {
  cex::common::Decimal speed;
  cex::common::Decimal lagrangian;
};

struct MonotoneInterpolationConfig {
  // Number of equal sub-intervals per segment [x_i, x_{i+1}].
  // 1 means no additional inner points.
  std::size_t subdivisions_per_segment{8};
};

struct ConvexificationConfig {
  // Optional working range by cumulative volume q.
  // If not set, whole S(q) range is convexified.
  std::optional<cex::common::Decimal> q_min;
  std::optional<cex::common::Decimal> q_max;
};

struct MoreauL2RegularizationConfig {
  // Smoothing strength mu in Moreau envelope:
  // larger value => stronger smoothing, less exact fit to the original.
  double smoothing{1e-3};
};

struct TikhonovL2RegularizationConfig {
  // Curvature penalty lambda:
  // larger value => smoother curve, less exact fit to the original.
  double curvature_penalty{1e-6};

  // Conjugate-gradient settings for (I + lambda D^T D) z = y.
  std::size_t max_iterations{256};
  double tolerance{1e-10};
};

struct FenchelLegendreConfig {
  // Optional explicit price grid range for dual layer S*(p).
  std::optional<cex::common::Decimal> p_min;
  std::optional<cex::common::Decimal> p_max;

  // Optional explicit price step. If not set, step is inferred from slope knots.
  std::optional<cex::common::Decimal> p_step;

  // Whether to include original marginal-price knots into dual grid.
  bool include_price_knots{true};
};

// Tabular representation of conjugate dual value S*(p).
struct SStarOfPPoint {
  cex::common::Decimal price;
  cex::common::Decimal dual_value;
};

// Tabular representation of CSLO-style optimizer output q*(p) = argmax_q (p*q - S(q)).
struct QStarOfPPoint {
  cex::common::Decimal price;
  cex::common::Decimal optimal_qty;
};

struct FenchelLegendreLayer {
  std::vector<SStarOfPPoint> s_star_of_p;
  std::vector<QStarOfPPoint> q_star_of_p;

  bool empty() const {
    return s_star_of_p.empty() || q_star_of_p.empty();
  }
};

struct DepthSideCurves {
  ExecutionSide side{ExecutionSide::kBuy};
  cex::common::Decimal tau_sec{1, 0};
  std::vector<QOfPPoint> q_of_p;
  std::vector<POfQPoint> p_of_q;
  std::vector<SOfQPoint> s_of_q;
  std::vector<LOfVPoint> l_of_v;
  std::vector<LOfVPoint> l_of_v_monotone;
  FenchelLegendreLayer fenchel_legendre;

  bool empty() const {
    return q_of_p.empty();
  }
};

// Builds Q(p) and p(q) from already canonical levels of the relevant side.
DepthSideCurves BuildDepthSideCurvesFromLevels(
    const std::vector<BookLevel>& levels,
    ExecutionSide side);

// Same as above but also builds S(q), v=q/tau and L(v)=S(v*tau)/tau.
DepthSideCurves BuildDepthSideCurvesFromLevels(
    const std::vector<BookLevel>& levels,
    ExecutionSide side,
    const cex::common::Decimal& tau_sec);

// Selects proper book side (asks for buy, bids for sell) and builds curves.
DepthSideCurves BuildDepthSideCurves(
    const CanonicalOrderBook& book,
    ExecutionSide side);

// Same as above with explicit tau.
DepthSideCurves BuildDepthSideCurves(
    const CanonicalOrderBook& book,
    ExecutionSide side,
    const cex::common::Decimal& tau_sec);

// Monotone shape-preserving interpolation for engineering L(v) curve.
std::vector<LOfVPoint> BuildMonotoneLagrangianInterpolation(
    const std::vector<LOfVPoint>& knots,
    const MonotoneInterpolationConfig& config = {});

// Convexifies integrated cost S(q) on a working volume range.
std::vector<SOfQPoint> ConvexifyCostLayer(
    const std::vector<SOfQPoint>& s_of_q,
    const ConvexificationConfig& config = {});

// Moreau envelope smoothing of integrated cost layer in discrete L2 form.
std::vector<SOfQPoint> RegularizeCostLayerMoreauL2(
    const std::vector<SOfQPoint>& s_of_q,
    const MoreauL2RegularizationConfig& config = {});

// Tikhonov/spline smoothing with L2 penalty on second differences.
std::vector<SOfQPoint> RegularizeCostLayerTikhonovL2(
    const std::vector<SOfQPoint>& s_of_q,
    const TikhonovL2RegularizationConfig& config = {});

// Builds tabular Fenchel/Legendre dual layer S*(p) and CSLO form q*(p).
FenchelLegendreLayer BuildFenchelLegendreLayer(
    const std::vector<SOfQPoint>& s_of_q,
    const FenchelLegendreConfig& config = {});

// Rebuilds p(q), q(p), L(v) from convexified S(q) layer.
DepthSideCurves ApplyConvexifiedCostLayer(
    const DepthSideCurves& curves,
    const ConvexificationConfig& config = {});

// Applies Moreau-L2 regularization and rebuilds derived tabular curves.
DepthSideCurves ApplyMoreauRegularizationL2(
    const DepthSideCurves& curves,
    const MoreauL2RegularizationConfig& config = {});

// Applies Tikhonov-L2 regularization and rebuilds derived tabular curves.
DepthSideCurves ApplyTikhonovRegularizationL2(
    const DepthSideCurves& curves,
    const TikhonovL2RegularizationConfig& config = {});

// Applies Fenchel/Legendre transform for current S(q) layer and stores dual tables.
DepthSideCurves ApplyFenchelLegendreLayer(
    const DepthSideCurves& curves,
    const FenchelLegendreConfig& config = {});

// Rebuilds derived tabular curves (p(q), q(p), L(v)) from custom S(q) layer.
DepthSideCurves RebuildFromCostLayer(
    const DepthSideCurves& curves,
    const std::vector<SOfQPoint>& s_of_q);

}  // namespace cex::venues::domain
