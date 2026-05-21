#include "domain/depth_curve_builder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace cex::venues::domain {

namespace {

constexpr int32_t kSpeedScale = 6;
constexpr int32_t kLagrangianScale = 6;
constexpr std::size_t kDefaultInterpolationSubdivisions = 8;
constexpr int32_t kConvexCostScale = 6;
constexpr int32_t kDualPriceScale = 6;
constexpr int32_t kDualValueScale = 6;

__int128 pow10_i128(const int32_t power) {
  __int128 out = 1;
  for (int32_t i = 0; i < power; ++i) out *= 10;
  return out;
}

cex::common::Decimal DivideDecimal(const cex::common::Decimal& numerator,
                                   const cex::common::Decimal& denominator,
                                   int32_t out_scale) {
  if (out_scale < 0) out_scale = 0;
  if (denominator.units == 0) return cex::common::Decimal{0, out_scale};

  __int128 scaled_num = numerator.units;
  __int128 scaled_den = denominator.units;

  const int32_t scale_delta = denominator.scale + out_scale - numerator.scale;
  if (scale_delta >= 0) {
    scaled_num *= pow10_i128(scale_delta);
  } else {
    scaled_den *= pow10_i128(-scale_delta);
  }

  const bool negative = (scaled_num < 0) != (scaled_den < 0);
  __int128 abs_num = (scaled_num < 0) ? -scaled_num : scaled_num;
  __int128 abs_den = (scaled_den < 0) ? -scaled_den : scaled_den;
  if (abs_den == 0) return cex::common::Decimal{0, out_scale};

  __int128 quotient = (abs_num + abs_den / 2) / abs_den;  // rounding half-up
  if (negative) quotient = -quotient;

  if (quotient > std::numeric_limits<int64_t>::max()) {
    quotient = std::numeric_limits<int64_t>::max();
  } else if (quotient < std::numeric_limits<int64_t>::min()) {
    quotient = std::numeric_limits<int64_t>::min();
  }

  return cex::common::Decimal{
      .units = static_cast<int64_t>(quotient),
      .scale = out_scale,
  };
}

cex::common::Decimal ToDecimalWithScale(const double value, const int32_t scale) {
  const double mul = std::pow(10.0, static_cast<double>(scale));
  const double scaled = std::round(value * mul);

  if (scaled > static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return cex::common::Decimal{std::numeric_limits<int64_t>::max(), scale};
  }
  if (scaled < static_cast<double>(std::numeric_limits<int64_t>::min())) {
    return cex::common::Decimal{std::numeric_limits<int64_t>::min(), scale};
  }
  return cex::common::Decimal{static_cast<int64_t>(scaled), scale};
}

cex::common::Decimal MonotonicPrice(const cex::common::Decimal& previous,
                                    const cex::common::Decimal& current,
                                    const ExecutionSide side) {
  if (side == ExecutionSide::kBuy) {
    return cex::common::Decimal::max(previous, current);   // non-decreasing
  }
  return cex::common::Decimal::min(previous, current);     // non-increasing
}

std::vector<LOfVPoint> BuildMonotoneLagrangianInterpolationImpl(
    const std::vector<LOfVPoint>& knots,
    std::size_t subdivisions_per_segment) {
  if (knots.size() <= 1) return knots;
  if (subdivisions_per_segment == 0) subdivisions_per_segment = 1;

  const std::size_t n = knots.size();
  std::vector<double> x(n, 0.0);
  std::vector<double> y(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = static_cast<double>(knots[i].speed);
    y[i] = static_cast<double>(knots[i].lagrangian);
  }

  std::vector<double> h(n - 1, 0.0);
  std::vector<double> d(n - 1, 0.0);
  for (std::size_t i = 0; i + 1 < n; ++i) {
    h[i] = x[i + 1] - x[i];
    if (h[i] <= 0.0) {
      // Degenerate or non-increasing grid: return original knots.
      return knots;
    }
    d[i] = (y[i + 1] - y[i]) / h[i];
  }

  // Fritsch-Carlson monotone slope selection (shape-preserving cubic Hermite).
  std::vector<double> m(n, 0.0);
  m[0] = d[0];
  m[n - 1] = d[n - 2];
  for (std::size_t i = 1; i + 1 < n; ++i) {
    if (d[i - 1] * d[i] <= 0.0) {
      m[i] = 0.0;
      continue;
    }
    const double w1 = 2.0 * h[i] + h[i - 1];
    const double w2 = h[i] + 2.0 * h[i - 1];
    m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
  }

  std::vector<LOfVPoint> out;
  out.reserve((n - 1) * subdivisions_per_segment + 1);

  auto push_point = [&out](const double x_value,
                           const double y_value,
                           bool enforce_monotone) {
    LOfVPoint point{
        .speed = ToDecimalWithScale(x_value, kSpeedScale),
        .lagrangian = ToDecimalWithScale(y_value, kLagrangianScale),
    };

    if (enforce_monotone && !out.empty() &&
        cex::common::Decimal::cmp(point.lagrangian, out.back().lagrangian) < 0) {
      point.lagrangian = out.back().lagrangian;
    }
    out.push_back(point);
  };

  push_point(x.front(), y.front(), false);

  for (std::size_t i = 0; i + 1 < n; ++i) {
    const double x0 = x[i];
    const double y0 = y[i];
    const double y1 = y[i + 1];
    const double hi = h[i];

    for (std::size_t s = 1; s <= subdivisions_per_segment; ++s) {
      const double t = static_cast<double>(s) /
                       static_cast<double>(subdivisions_per_segment);

      const double t2 = t * t;
      const double t3 = t2 * t;
      const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
      const double h10 = t3 - 2.0 * t2 + t;
      const double h01 = -2.0 * t3 + 3.0 * t2;
      const double h11 = t3 - t2;

      const double x_value = x0 + t * hi;
      double y_value = h00 * y0 + h10 * hi * m[i] + h01 * y1 + h11 * hi * m[i + 1];

      // Shape-preserving safety clamp per segment (no overshoot).
      const double low = std::min(y0, y1);
      const double high = std::max(y0, y1);
      y_value = std::clamp(y_value, low, high);

      push_point(x_value, y_value, true);
    }
  }

  return out;
}

struct ConvexRange {
  std::size_t begin{0};  // point index inclusive
  std::size_t end{0};    // point index inclusive
  bool valid{false};
};

ConvexRange SelectConvexRange(const std::vector<SOfQPoint>& s_of_q,
                              const ConvexificationConfig& config) {
  ConvexRange range;
  if (s_of_q.size() < 2) return range;

  std::size_t begin = 0;
  std::size_t end = s_of_q.size() - 1;

  if (config.q_min.has_value()) {
    bool found = false;
    for (std::size_t i = 0; i < s_of_q.size(); ++i) {
      if (cex::common::Decimal::cmp(s_of_q[i].qty, *config.q_min) >= 0) {
        begin = i;
        found = true;
        break;
      }
    }
    if (!found) return range;
  }

  if (config.q_max.has_value()) {
    bool found = false;
    for (std::size_t i = s_of_q.size(); i > 0; --i) {
      const std::size_t idx = i - 1;
      if (cex::common::Decimal::cmp(s_of_q[idx].qty, *config.q_max) <= 0) {
        end = idx;
        found = true;
        break;
      }
    }
    if (!found) return range;
  }

  if (begin >= end) return range;

  range.begin = begin;
  range.end = end;
  range.valid = true;
  return range;
}

std::vector<double> ConvexifySlopesPav(const std::vector<double>& slopes,
                                       const std::vector<double>& weights) {
  struct Block {
    std::size_t left{0};
    std::size_t right{0};
    double weight_sum{0.0};
    double weighted_slope_sum{0.0};
  };

  std::vector<Block> blocks;
  blocks.reserve(slopes.size());

  for (std::size_t i = 0; i < slopes.size(); ++i) {
    Block b{
        .left = i,
        .right = i,
        .weight_sum = weights[i],
        .weighted_slope_sum = weights[i] * slopes[i],
    };
    blocks.push_back(b);

    while (blocks.size() >= 2) {
      const auto& b2 = blocks.back();
      const auto& b1 = blocks[blocks.size() - 2];
      const double m1 = b1.weighted_slope_sum / b1.weight_sum;
      const double m2 = b2.weighted_slope_sum / b2.weight_sum;
      if (m1 <= m2) break;

      Block merged{
          .left = b1.left,
          .right = b2.right,
          .weight_sum = b1.weight_sum + b2.weight_sum,
          .weighted_slope_sum = b1.weighted_slope_sum + b2.weighted_slope_sum,
      };
      blocks.pop_back();
      blocks.back() = merged;
    }
  }

  std::vector<double> out(slopes.size(), 0.0);
  for (const auto& block : blocks) {
    const double mean = block.weighted_slope_sum / block.weight_sum;
    for (std::size_t i = block.left; i <= block.right; ++i) {
      out[i] = mean;
    }
  }
  return out;
}

std::vector<SOfQPoint> ConvexifyCostLayerImpl(
    const std::vector<SOfQPoint>& s_of_q,
    const ConvexificationConfig& config) {
  if (s_of_q.size() < 3) return s_of_q;

  std::vector<SOfQPoint> out = s_of_q;
  const ConvexRange range = SelectConvexRange(s_of_q, config);
  if (!range.valid) return out;

  const std::size_t point_count = range.end - range.begin + 1;
  if (point_count < 3) return out;

  std::vector<double> x(point_count, 0.0);
  std::vector<double> y(point_count, 0.0);
  int32_t out_scale = 0;
  for (std::size_t i = 0; i < point_count; ++i) {
    const auto& p = s_of_q[range.begin + i];
    x[i] = static_cast<double>(p.qty);
    y[i] = static_cast<double>(p.cumulative_cost);
    out_scale = std::max(out_scale, p.cumulative_cost.scale);
  }
  out_scale = std::max(out_scale, kConvexCostScale);

  std::vector<double> h(point_count - 1, 0.0);
  std::vector<double> slopes(point_count - 1, 0.0);
  for (std::size_t i = 0; i + 1 < point_count; ++i) {
    h[i] = x[i + 1] - x[i];
    if (h[i] <= 0.0) return out;  // invalid grid
    slopes[i] = (y[i + 1] - y[i]) / h[i];
  }

  const std::vector<double> convex_slopes = ConvexifySlopesPav(slopes, h);

  std::vector<double> y_conv(point_count, 0.0);
  y_conv[0] = y[0];
  for (std::size_t i = 0; i + 1 < point_count; ++i) {
    y_conv[i + 1] = y_conv[i] + convex_slopes[i] * h[i];
  }

  for (std::size_t i = 0; i < point_count; ++i) {
    out[range.begin + i].cumulative_cost =
        ToDecimalWithScale(y_conv[i], out_scale);
  }

  return out;
}

int32_t DetectCostOutputScale(const std::vector<SOfQPoint>& s_of_q) {
  int32_t out_scale = kConvexCostScale;
  double max_abs_cost = 0.0;
  for (const auto& p : s_of_q) {
    out_scale = std::max(out_scale, p.cumulative_cost.scale);
    max_abs_cost = std::max(max_abs_cost, std::fabs(static_cast<double>(p.cumulative_cost)));
  }

  if (!std::isfinite(max_abs_cost) || max_abs_cost <= 0.0) {
    return out_scale;
  }

  while (out_scale > 0) {
    const long double scaled = static_cast<long double>(max_abs_cost) *
                               std::pow(10.0L, static_cast<long double>(out_scale));
    if (scaled <= static_cast<long double>(std::numeric_limits<int64_t>::max())) {
      break;
    }
    --out_scale;
  }
  return out_scale;
}

std::vector<double> ExtractQtyGrid(const std::vector<SOfQPoint>& s_of_q) {
  std::vector<double> x;
  x.reserve(s_of_q.size());
  for (const auto& p : s_of_q) {
    x.push_back(static_cast<double>(p.qty));
  }
  return x;
}

std::vector<double> ExtractCostValues(const std::vector<SOfQPoint>& s_of_q) {
  std::vector<double> y;
  y.reserve(s_of_q.size());
  for (const auto& p : s_of_q) {
    y.push_back(static_cast<double>(p.cumulative_cost));
  }
  return y;
}

cex::common::Decimal SellSideReferencePrice(const DepthSideCurves& curves) {
  if (!curves.p_of_q.empty()) {
    return curves.p_of_q.front().price;
  }
  if (!curves.q_of_p.empty()) {
    return curves.q_of_p.front().price;
  }
  return cex::common::Decimal{0, 0};
}

std::vector<SOfQPoint> NormalizeCostLayerForL2(const DepthSideCurves& curves) {
  if (curves.side != ExecutionSide::kSell) {
    return curves.s_of_q;
  }

  const cex::common::Decimal reference_price = SellSideReferencePrice(curves);
  std::vector<SOfQPoint> out = curves.s_of_q;
  for (auto& point : out) {
    const cex::common::Decimal linear_cost =
        cex::common::Decimal::mul(reference_price, point.qty);
    point.cumulative_cost = cex::common::Decimal::sub(linear_cost, point.cumulative_cost);
  }
  return out;
}

std::vector<SOfQPoint> DenormalizeCostLayerFromL2(const DepthSideCurves& curves,
                                                  const std::vector<SOfQPoint>& s_of_q) {
  if (curves.side != ExecutionSide::kSell) {
    return s_of_q;
  }

  const cex::common::Decimal reference_price = SellSideReferencePrice(curves);
  std::vector<SOfQPoint> out = s_of_q;
  for (auto& point : out) {
    const cex::common::Decimal linear_cost =
        cex::common::Decimal::mul(reference_price, point.qty);
    point.cumulative_cost = cex::common::Decimal::sub(linear_cost, point.cumulative_cost);
  }
  return out;
}

void EnforceNonDecreasing(std::vector<double>* values);

int32_t DetectQtyOutputScale(const std::vector<SOfQPoint>& s_of_q) {
  int32_t out_scale = 0;
  for (const auto& p : s_of_q) {
    out_scale = std::max(out_scale, p.qty.scale);
  }
  return out_scale;
}

std::vector<double> ExtractSlopeKnots(const std::vector<SOfQPoint>& s_of_q) {
  if (s_of_q.size() < 2) return {};
  std::vector<double> slopes;
  slopes.reserve(s_of_q.size() - 1);

  for (std::size_t i = 1; i < s_of_q.size(); ++i) {
    const double dq = static_cast<double>(s_of_q[i].qty) -
                      static_cast<double>(s_of_q[i - 1].qty);
    if (dq <= 0.0) continue;
    const double ds = static_cast<double>(s_of_q[i].cumulative_cost) -
                      static_cast<double>(s_of_q[i - 1].cumulative_cost);
    slopes.push_back(ds / dq);
  }
  return slopes;
}

std::vector<double> UniqueSortedWithTolerance(std::vector<double> values,
                                              const double eps) {
  if (values.empty()) return values;
  std::sort(values.begin(), values.end());

  std::vector<double> out;
  out.reserve(values.size());
  out.push_back(values.front());
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (std::fabs(values[i] - out.back()) > eps) {
      out.push_back(values[i]);
    }
  }
  return out;
}

double InferFenchelStep(const std::vector<double>& slope_knots) {
  if (slope_knots.size() < 2) return 1.0;
  const std::vector<double> unique = UniqueSortedWithTolerance(slope_knots, 1e-12);
  if (unique.size() < 2) return 1.0;

  double step = std::numeric_limits<double>::infinity();
  for (std::size_t i = 1; i < unique.size(); ++i) {
    const double d = unique[i] - unique[i - 1];
    if (d > 1e-12) step = std::min(step, d);
  }
  if (!std::isfinite(step) || step <= 0.0) return 1.0;
  return step;
}

std::vector<double> BuildFenchelPriceGrid(const std::vector<SOfQPoint>& s_of_q,
                                          const FenchelLegendreConfig& config) {
  const std::vector<double> slope_knots = ExtractSlopeKnots(s_of_q);

  double p_min = 0.0;
  double p_max = 0.0;

  if (config.p_min.has_value()) {
    p_min = static_cast<double>(*config.p_min);
  } else if (!slope_knots.empty()) {
    p_min = *std::min_element(slope_knots.begin(), slope_knots.end());
  }

  if (config.p_max.has_value()) {
    p_max = static_cast<double>(*config.p_max);
  } else if (!slope_knots.empty()) {
    p_max = *std::max_element(slope_knots.begin(), slope_knots.end());
  } else {
    p_max = p_min;
  }

  if (p_max < p_min) std::swap(p_min, p_max);

  double step = 0.0;
  if (config.p_step.has_value()) {
    step = std::fabs(static_cast<double>(*config.p_step));
  }
  if (step <= 0.0) step = InferFenchelStep(slope_knots);
  if (step <= 0.0) step = 1.0;

  std::vector<double> grid;
  if (std::fabs(p_max - p_min) <= 1e-12) {
    grid.push_back(p_min);
  } else {
    const std::size_t kMaxGridSize = 20000;
    double p = p_min;
    while (p <= p_max + 1e-12 && grid.size() < kMaxGridSize) {
      grid.push_back(p);
      p += step;
    }
    if (grid.empty() || std::fabs(grid.back() - p_max) > 1e-9) {
      grid.push_back(p_max);
    }
  }

  if (config.include_price_knots) {
    for (const double slope : slope_knots) {
      if (slope >= p_min - 1e-12 && slope <= p_max + 1e-12) {
        grid.push_back(slope);
      }
    }
  }

  return UniqueSortedWithTolerance(std::move(grid), 1e-10);
}

FenchelLegendreLayer BuildFenchelLegendreLayerImpl(
    const std::vector<SOfQPoint>& s_of_q,
    const FenchelLegendreConfig& config) {
  FenchelLegendreLayer out;
  if (s_of_q.empty()) return out;

  const std::vector<double> x = ExtractQtyGrid(s_of_q);
  const std::vector<double> y = ExtractCostValues(s_of_q);
  const std::vector<double> price_grid = BuildFenchelPriceGrid(s_of_q, config);
  if (price_grid.empty()) return out;

  std::vector<double> dual_values(price_grid.size(), 0.0);
  std::vector<double> argmax_qty(price_grid.size(), 0.0);

  for (std::size_t pi = 0; pi < price_grid.size(); ++pi) {
    const double p = price_grid[pi];
    std::size_t best_idx = 0;
    double best_value = p * x[0] - y[0];

    for (std::size_t qi = 1; qi < x.size(); ++qi) {
      const double candidate = p * x[qi] - y[qi];
      if (candidate > best_value + 1e-9) {
        best_value = candidate;
        best_idx = qi;
        continue;
      }
      if (std::fabs(candidate - best_value) <= 1e-9 && x[qi] > x[best_idx]) {
        best_idx = qi;
      }
    }

    dual_values[pi] = best_value;
    argmax_qty[pi] = x[best_idx];
  }

  EnforceNonDecreasing(&argmax_qty);

  const int32_t dual_scale = std::max(DetectCostOutputScale(s_of_q), kDualValueScale);
  const int32_t qty_scale = DetectQtyOutputScale(s_of_q);

  out.s_star_of_p.reserve(price_grid.size());
  out.q_star_of_p.reserve(price_grid.size());
  for (std::size_t i = 0; i < price_grid.size(); ++i) {
    const cex::common::Decimal p = ToDecimalWithScale(price_grid[i], kDualPriceScale);
    out.s_star_of_p.push_back(SStarOfPPoint{
        .price = p,
        .dual_value = ToDecimalWithScale(dual_values[i], dual_scale),
    });
    out.q_star_of_p.push_back(QStarOfPPoint{
        .price = p,
        .optimal_qty = ToDecimalWithScale(argmax_qty[i], qty_scale),
    });
  }

  return out;
}

void EnforceNonDecreasing(std::vector<double>* values) {
  if (values == nullptr || values->empty()) return;
  for (std::size_t i = 1; i < values->size(); ++i) {
    if ((*values)[i] < (*values)[i - 1]) {
      (*values)[i] = (*values)[i - 1];
    }
  }
}

double DotProduct(const std::vector<double>& lhs, const std::vector<double>& rhs) {
  if (lhs.size() != rhs.size()) return 0.0;
  double out = 0.0;
  for (std::size_t i = 0; i < lhs.size(); ++i) out += lhs[i] * rhs[i];
  return out;
}

void ApplySecondDifferenceNormalMatrix(const std::vector<double>& z,
                                       std::vector<double>* out) {
  if (out == nullptr) return;
  out->assign(z.size(), 0.0);
  if (z.size() < 3) return;

  // D row k: [ ..., 1, -2, 1, ... ] at columns k, k+1, k+2
  // Compute D^T D z without explicitly building dense matrix.
  for (std::size_t k = 0; k + 2 < z.size(); ++k) {
    const double d2 = z[k] - 2.0 * z[k + 1] + z[k + 2];
    (*out)[k] += d2;
    (*out)[k + 1] -= 2.0 * d2;
    (*out)[k + 2] += d2;
  }
}

void ApplyTikhonovSystemMatrix(const std::vector<double>& z,
                               const double lambda,
                               std::vector<double>* out) {
  if (out == nullptr) return;
  std::vector<double> dtd;
  ApplySecondDifferenceNormalMatrix(z, &dtd);

  out->assign(z.size(), 0.0);
  for (std::size_t i = 0; i < z.size(); ++i) {
    (*out)[i] = z[i] + lambda * dtd[i];
  }
}

std::vector<double> SolveTikhonovCg(const std::vector<double>& y,
                                    const double lambda,
                                    const std::size_t max_iterations,
                                    const double tolerance) {
  if (y.size() < 3 || lambda <= 0.0) return y;

  std::vector<double> z = y;
  std::vector<double> az;
  ApplyTikhonovSystemMatrix(z, lambda, &az);

  std::vector<double> r(y.size(), 0.0);
  for (std::size_t i = 0; i < y.size(); ++i) r[i] = y[i] - az[i];

  std::vector<double> p = r;
  double rr = DotProduct(r, r);
  const double y_norm = std::max(1.0, DotProduct(y, y));
  const double tol2 = std::max(0.0, tolerance) * std::max(0.0, tolerance) * y_norm;
  if (rr <= tol2) return z;

  std::vector<double> ap;
  for (std::size_t iter = 0; iter < std::max<std::size_t>(1, max_iterations); ++iter) {
    ApplyTikhonovSystemMatrix(p, lambda, &ap);
    const double denom = DotProduct(p, ap);
    if (std::fabs(denom) <= 1e-20) break;

    const double alpha = rr / denom;
    for (std::size_t i = 0; i < z.size(); ++i) {
      z[i] += alpha * p[i];
      r[i] -= alpha * ap[i];
    }

    const double rr_new = DotProduct(r, r);
    if (rr_new <= tol2) break;
    const double beta = rr_new / rr;
    for (std::size_t i = 0; i < p.size(); ++i) {
      p[i] = r[i] + beta * p[i];
    }
    rr = rr_new;
  }
  return z;
}

std::vector<SOfQPoint> RegularizeCostLayerMoreauL2Impl(
    const std::vector<SOfQPoint>& s_of_q,
    const MoreauL2RegularizationConfig& config) {
  if (s_of_q.size() < 2) return s_of_q;

  const double mu = config.smoothing;
  if (mu <= 0.0) return s_of_q;

  const std::vector<double> x = ExtractQtyGrid(s_of_q);
  const std::vector<double> y = ExtractCostValues(s_of_q);
  double reference_price = 0.0;
  for (std::size_t i = 1; i < s_of_q.size(); ++i) {
    const double dq = x[i] - x[i - 1];
    if (dq <= 1e-12) continue;
    const double ds = y[i] - y[i - 1];
    reference_price = std::max(reference_price, std::fabs(ds / dq));
  }
  if (!std::isfinite(reference_price) || reference_price <= 0.0) {
    const double qty_span = std::max(1e-9, x.back() - x.front());
    reference_price = std::max(1.0, std::fabs(y.back() - y.front()) / qty_span);
  }
  const double effective_mu = std::max(1e-12, mu / reference_price);

  std::vector<double> y_reg(y.size(), 0.0);

  // Discrete Moreau envelope on cost-consistent units:
  // f_mu(x_i) = min_j { f(x_j) + (x_i - x_j)^2 / (2*mu) }.
  //
  // S(q) is denominated in quote currency while q is denominated in base
  // quantity. Treat the configured smoothing as dimensionless and rescale it
  // by a local reference marginal price so the quadratic penalty is comparable
  // to the monetary cost layer on high-price instruments.
  for (std::size_t i = 0; i < y.size(); ++i) {
    double best = y[i];
    for (std::size_t j = 0; j < y.size(); ++j) {
      const double dx = x[i] - x[j];
      const double candidate = y[j] + (dx * dx) / (2.0 * effective_mu);
      if (candidate < best) best = candidate;
    }
    y_reg[i] = best;
  }

  y_reg.front() = y.front();
  // Integrated cost must stay non-decreasing by cumulative volume.
  EnforceNonDecreasing(&y_reg);

  std::vector<SOfQPoint> out = s_of_q;
  const int32_t out_scale = DetectCostOutputScale(s_of_q);
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i].cumulative_cost = ToDecimalWithScale(y_reg[i], out_scale);
  }
  return out;
}

std::vector<SOfQPoint> RegularizeCostLayerTikhonovL2Impl(
    const std::vector<SOfQPoint>& s_of_q,
    const TikhonovL2RegularizationConfig& config) {
  if (s_of_q.size() < 3) return s_of_q;

  const double lambda = config.curvature_penalty;
  if (lambda <= 0.0) return s_of_q;

  const std::vector<double> y = ExtractCostValues(s_of_q);
  std::vector<double> y_reg = SolveTikhonovCg(
      y,
      lambda,
      config.max_iterations,
      config.tolerance);
  if (y_reg.size() != y.size()) return s_of_q;

  // Keep exact starting anchor S(0), and preserve monotonic integrated cost.
  y_reg.front() = y.front();
  EnforceNonDecreasing(&y_reg);

  std::vector<SOfQPoint> out = s_of_q;
  const int32_t out_scale = DetectCostOutputScale(s_of_q);
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i].cumulative_cost = ToDecimalWithScale(y_reg[i], out_scale);
  }
  return out;
}

DepthSideCurves RebuildCurvesFromCostLayer(const DepthSideCurves& curves,
                                           const std::vector<SOfQPoint>& s_of_q) {
  DepthSideCurves out = curves;
  out.s_of_q = s_of_q;
  out.fenchel_legendre = {};

  if (s_of_q.empty()) {
    out.q_of_p.clear();
    out.p_of_q.clear();
    out.l_of_v.clear();
    out.l_of_v_monotone.clear();
    return out;
  }

  int32_t price_scale = 0;
  for (const auto& p : curves.p_of_q) {
    price_scale = std::max(price_scale, p.price.scale);
  }
  price_scale = std::max(price_scale, 6);

  out.p_of_q.clear();
  out.q_of_p.clear();
  out.l_of_v.clear();
  out.l_of_v_monotone.clear();

  out.p_of_q.resize(s_of_q.size());

  // Rebuild marginal p(q) from convexified S(q) slopes.
  for (std::size_t i = 1; i < s_of_q.size(); ++i) {
    const cex::common::Decimal dq =
        cex::common::Decimal::sub(s_of_q[i].qty, s_of_q[i - 1].qty);
    const cex::common::Decimal ds =
        cex::common::Decimal::sub(s_of_q[i].cumulative_cost, s_of_q[i - 1].cumulative_cost);

    cex::common::Decimal price = DivideDecimal(ds, dq, price_scale);
    if (out.side == ExecutionSide::kBuy && i > 1 &&
        cex::common::Decimal::cmp(price, out.p_of_q[i - 1].price) < 0) {
      price = out.p_of_q[i - 1].price;
    }
    if (out.side == ExecutionSide::kSell && i > 1 &&
        cex::common::Decimal::cmp(price, out.p_of_q[i - 1].price) > 0) {
      price = out.p_of_q[i - 1].price;
    }

    out.p_of_q[i] = POfQPoint{
        .qty = s_of_q[i].qty,
        .price = price,
    };
  }

  out.p_of_q[0] = POfQPoint{
      .qty = s_of_q[0].qty,
      .price = (s_of_q.size() > 1) ? out.p_of_q[1].price
                                   : cex::common::Decimal{0, price_scale},
  };

  if (s_of_q.size() > 1) {
    out.q_of_p.reserve(s_of_q.size() - 1);
    for (std::size_t i = 1; i < s_of_q.size(); ++i) {
      out.q_of_p.push_back(QOfPPoint{
          .price = out.p_of_q[i].price,
          .cumulative_qty = s_of_q[i].qty,
      });
    }
  }

  const cex::common::Decimal tau =
      (out.tau_sec.units > 0) ? out.tau_sec : cex::common::Decimal{1, 0};
  out.l_of_v.reserve(s_of_q.size());
  for (const auto& s_point : s_of_q) {
    out.l_of_v.push_back(LOfVPoint{
        .speed = DivideDecimal(s_point.qty, tau, kSpeedScale),
        .lagrangian = DivideDecimal(s_point.cumulative_cost, tau, kLagrangianScale),
    });
  }

  out.l_of_v_monotone =
      BuildMonotoneLagrangianInterpolationImpl(
          out.l_of_v,
          kDefaultInterpolationSubdivisions);

  return out;
}

DepthSideCurves BuildDepthSideCurvesImpl(const std::vector<BookLevel>& levels,
                                         const ExecutionSide side,
                                         const cex::common::Decimal& tau_sec) {
  DepthSideCurves out;
  out.side = side;
  out.tau_sec = (tau_sec.units > 0) ? tau_sec : cex::common::Decimal{1, 0};

  if (levels.empty()) return out;

  const cex::common::Decimal first_increment_cost =
      cex::common::Decimal::mul(levels.front().price, levels.front().qty);
  cex::common::Decimal cumulative_qty{
      .units = 0,
      .scale = levels.front().qty.scale,
  };
  cex::common::Decimal cumulative_cost{
      .units = 0,
      .scale = std::max(0, first_increment_cost.scale),
  };
  cex::common::Decimal monotonic_price = levels.front().price;

  const cex::common::Decimal zero_qty{
      .units = 0,
      .scale = cumulative_qty.scale,
  };

  // Convention for Level-1 tabular p(q): p(0) = best executable level price.
  out.p_of_q.push_back(POfQPoint{
      .qty = cumulative_qty,
      .price = monotonic_price,
  });
  out.s_of_q.push_back(SOfQPoint{
      .qty = zero_qty,
      .cumulative_cost = cumulative_cost,
  });
  out.l_of_v.push_back(LOfVPoint{
      .speed = DivideDecimal(zero_qty, out.tau_sec, kSpeedScale),
      .lagrangian = DivideDecimal(cumulative_cost, out.tau_sec, kLagrangianScale),
  });

  out.q_of_p.reserve(levels.size());
  out.p_of_q.reserve(levels.size() + 1);
  out.s_of_q.reserve(levels.size() + 1);
  out.l_of_v.reserve(levels.size() + 1);

  for (const auto& level : levels) {
    monotonic_price = MonotonicPrice(monotonic_price, level.price, side);
    cumulative_qty = cex::common::Decimal::add(cumulative_qty, level.qty);
    cumulative_cost = cex::common::Decimal::add(
        cumulative_cost,
        cex::common::Decimal::mul(monotonic_price, level.qty));

    out.q_of_p.push_back(QOfPPoint{
        .price = monotonic_price,
        .cumulative_qty = cumulative_qty,
    });

    out.p_of_q.push_back(POfQPoint{
        .qty = cumulative_qty,
        .price = monotonic_price,
    });

    out.s_of_q.push_back(SOfQPoint{
        .qty = cumulative_qty,
        .cumulative_cost = cumulative_cost,
    });

    out.l_of_v.push_back(LOfVPoint{
        .speed = DivideDecimal(cumulative_qty, out.tau_sec, kSpeedScale),
        .lagrangian = DivideDecimal(cumulative_cost, out.tau_sec, kLagrangianScale),
    });

    if (out.l_of_v.size() >= 2 &&
        cex::common::Decimal::cmp(out.l_of_v.back().lagrangian,
                                  out.l_of_v[out.l_of_v.size() - 2].lagrangian) < 0) {
      out.l_of_v.back().lagrangian = out.l_of_v[out.l_of_v.size() - 2].lagrangian;
    }
  }

  out.l_of_v_monotone =
      BuildMonotoneLagrangianInterpolationImpl(
          out.l_of_v,
          kDefaultInterpolationSubdivisions);

  return out;
}

}  // namespace

DepthSideCurves BuildDepthSideCurvesFromLevels(const std::vector<BookLevel>& levels,
                                               const ExecutionSide side) {
  return BuildDepthSideCurvesImpl(levels, side, cex::common::Decimal{1, 0});
}

DepthSideCurves BuildDepthSideCurvesFromLevels(
    const std::vector<BookLevel>& levels,
    const ExecutionSide side,
    const cex::common::Decimal& tau_sec) {
  return BuildDepthSideCurvesImpl(levels, side, tau_sec);
}

DepthSideCurves BuildDepthSideCurves(const CanonicalOrderBook& book,
                                     const ExecutionSide side) {
  const auto& side_levels = (side == ExecutionSide::kBuy) ? book.asks : book.bids;
  return BuildDepthSideCurvesFromLevels(side_levels, side, cex::common::Decimal{1, 0});
}

DepthSideCurves BuildDepthSideCurves(const CanonicalOrderBook& book,
                                     const ExecutionSide side,
                                     const cex::common::Decimal& tau_sec) {
  const auto& side_levels = (side == ExecutionSide::kBuy) ? book.asks : book.bids;
  return BuildDepthSideCurvesFromLevels(side_levels, side, tau_sec);
}

std::vector<LOfVPoint> BuildMonotoneLagrangianInterpolation(
    const std::vector<LOfVPoint>& knots,
    const MonotoneInterpolationConfig& config) {
  return BuildMonotoneLagrangianInterpolationImpl(
      knots,
      config.subdivisions_per_segment);
}

std::vector<SOfQPoint> ConvexifyCostLayer(
    const std::vector<SOfQPoint>& s_of_q,
    const ConvexificationConfig& config) {
  return ConvexifyCostLayerImpl(s_of_q, config);
}

std::vector<SOfQPoint> RegularizeCostLayerMoreauL2(
    const std::vector<SOfQPoint>& s_of_q,
    const MoreauL2RegularizationConfig& config) {
  return RegularizeCostLayerMoreauL2Impl(s_of_q, config);
}

std::vector<SOfQPoint> RegularizeCostLayerTikhonovL2(
    const std::vector<SOfQPoint>& s_of_q,
    const TikhonovL2RegularizationConfig& config) {
  return RegularizeCostLayerTikhonovL2Impl(s_of_q, config);
}

FenchelLegendreLayer BuildFenchelLegendreLayer(
    const std::vector<SOfQPoint>& s_of_q,
    const FenchelLegendreConfig& config) {
  return BuildFenchelLegendreLayerImpl(s_of_q, config);
}

DepthSideCurves ApplyConvexifiedCostLayer(
    const DepthSideCurves& curves,
    const ConvexificationConfig& config) {
  const std::vector<SOfQPoint> input_s = NormalizeCostLayerForL2(curves);
  const std::vector<SOfQPoint> convex_s = ConvexifyCostLayerImpl(input_s, config);
  return RebuildCurvesFromCostLayer(curves, DenormalizeCostLayerFromL2(curves, convex_s));
}

DepthSideCurves ApplyMoreauRegularizationL2(
    const DepthSideCurves& curves,
    const MoreauL2RegularizationConfig& config) {
  const std::vector<SOfQPoint> input_s = NormalizeCostLayerForL2(curves);
  const std::vector<SOfQPoint> regularized =
      RegularizeCostLayerMoreauL2Impl(input_s, config);
  return RebuildCurvesFromCostLayer(curves, DenormalizeCostLayerFromL2(curves, regularized));
}

DepthSideCurves ApplyTikhonovRegularizationL2(
    const DepthSideCurves& curves,
    const TikhonovL2RegularizationConfig& config) {
  const std::vector<SOfQPoint> input_s = NormalizeCostLayerForL2(curves);
  const std::vector<SOfQPoint> regularized =
      RegularizeCostLayerTikhonovL2Impl(input_s, config);
  return RebuildCurvesFromCostLayer(curves, DenormalizeCostLayerFromL2(curves, regularized));
}

DepthSideCurves ApplyFenchelLegendreLayer(
    const DepthSideCurves& curves,
    const FenchelLegendreConfig& config) {
  DepthSideCurves out = curves;
  out.fenchel_legendre = BuildFenchelLegendreLayerImpl(curves.s_of_q, config);
  return out;
}

DepthSideCurves RebuildFromCostLayer(
    const DepthSideCurves& curves,
    const std::vector<SOfQPoint>& s_of_q) {
  return RebuildCurvesFromCostLayer(curves, s_of_q);
}

}  // namespace cex::venues::domain
