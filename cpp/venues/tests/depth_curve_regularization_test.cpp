#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "domain/depth_curve_builder.hpp"

namespace {

using cex::common::Decimal;
using cex::venues::domain::ApplyMoreauRegularizationL2;
using cex::venues::domain::ApplyTikhonovRegularizationL2;
using cex::venues::domain::BookLevel;
using cex::venues::domain::BuildDepthSideCurvesFromLevels;
using cex::venues::domain::ExecutionSide;
using cex::venues::domain::MoreauL2RegularizationConfig;
using cex::venues::domain::RegularizeCostLayerMoreauL2;
using cex::venues::domain::RegularizeCostLayerTikhonovL2;
using cex::venues::domain::SOfQPoint;
using cex::venues::domain::TikhonovL2RegularizationConfig;

BookLevel Level(const int64_t price_units,
                const int64_t qty_units,
                const int32_t price_scale = 0,
                const int32_t qty_scale = 3) {
  return BookLevel{
      .price = Decimal{.units = price_units, .scale = price_scale},
      .qty = Decimal{.units = qty_units, .scale = qty_scale},
  };
}

SOfQPoint CostPoint(const int64_t q_units,
                    const int64_t s_units,
                    const int32_t scale = 0) {
  return SOfQPoint{
      .qty = Decimal{.units = q_units, .scale = scale},
      .cumulative_cost = Decimal{.units = s_units, .scale = scale},
  };
}

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

double Rmse(const std::vector<SOfQPoint>& lhs,
            const std::vector<SOfQPoint>& rhs) {
  if (lhs.size() != rhs.size() || lhs.empty()) return 0.0;
  double sum = 0.0;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    const double d = static_cast<double>(lhs[i].cumulative_cost) -
                     static_cast<double>(rhs[i].cumulative_cost);
    sum += d * d;
  }
  return std::sqrt(sum / static_cast<double>(lhs.size()));
}

double CurvatureEnergy(const std::vector<SOfQPoint>& s_of_q) {
  if (s_of_q.size() < 3) return 0.0;
  double out = 0.0;
  for (std::size_t i = 1; i + 1 < s_of_q.size(); ++i) {
    const double d2 = static_cast<double>(s_of_q[i - 1].cumulative_cost) -
                      2.0 * static_cast<double>(s_of_q[i].cumulative_cost) +
                      static_cast<double>(s_of_q[i + 1].cumulative_cost);
    out += d2 * d2;
  }
  return out;
}

bool IsNonDecreasing(const std::vector<SOfQPoint>& s_of_q) {
  for (std::size_t i = 1; i < s_of_q.size(); ++i) {
    if (cex::common::Decimal::cmp(s_of_q[i].cumulative_cost,
                                  s_of_q[i - 1].cumulative_cost) < 0) {
      return false;
    }
  }
  return true;
}

bool TestMoreauL2ParameterEffect() {
  // Piecewise cost with a sharp kink around q=3.
  const std::vector<SOfQPoint> src = {
      CostPoint(0, 0),
      CostPoint(1, 10),
      CostPoint(2, 21),
      CostPoint(3, 80),
      CostPoint(4, 90),
      CostPoint(5, 101),
      CostPoint(6, 112),
  };

  const auto low_mu = RegularizeCostLayerMoreauL2(
      src,
      MoreauL2RegularizationConfig{.smoothing = 0.005});
  const auto high_mu = RegularizeCostLayerMoreauL2(
      src,
      MoreauL2RegularizationConfig{.smoothing = 2.0});

  if (!Check(low_mu.size() == src.size(), "Moreau low-mu size mismatch")) return false;
  if (!Check(high_mu.size() == src.size(), "Moreau high-mu size mismatch")) return false;
  if (!Check(IsNonDecreasing(low_mu), "Moreau low-mu must be non-decreasing")) return false;
  if (!Check(IsNonDecreasing(high_mu), "Moreau high-mu must be non-decreasing")) return false;

  // Envelope must not exceed the original cost at grid points.
  for (std::size_t i = 0; i < src.size(); ++i) {
    if (!Check(cex::common::Decimal::cmp(low_mu[i].cumulative_cost,
                                         src[i].cumulative_cost) <= 0,
               "Moreau low-mu should not exceed source")) {
      return false;
    }
    if (!Check(cex::common::Decimal::cmp(high_mu[i].cumulative_cost,
                                         src[i].cumulative_cost) <= 0,
               "Moreau high-mu should not exceed source")) {
      return false;
    }
  }

  const double rmse_low = Rmse(src, low_mu);
  const double rmse_high = Rmse(src, high_mu);
  if (!Check(rmse_high > rmse_low,
             "Moreau: stronger smoothing must increase approximation error")) {
    return false;
  }

  // Curvature should become smoother when smoothing is stronger.
  if (!Check(CurvatureEnergy(high_mu) < CurvatureEnergy(low_mu),
             "Moreau: stronger smoothing must reduce curvature energy")) {
    return false;
  }

  return true;
}

bool TestTikhonovL2ParameterEffect() {
  const std::vector<SOfQPoint> src = {
      CostPoint(0, 0),
      CostPoint(1, 10),
      CostPoint(2, 30),
      CostPoint(3, 31),
      CostPoint(4, 60),
      CostPoint(5, 63),
      CostPoint(6, 90),
  };

  const auto low_lambda = RegularizeCostLayerTikhonovL2(
      src,
      TikhonovL2RegularizationConfig{
          .curvature_penalty = 0.01,
          .max_iterations = 256,
          .tolerance = 1e-12,
      });

  const auto high_lambda = RegularizeCostLayerTikhonovL2(
      src,
      TikhonovL2RegularizationConfig{
          .curvature_penalty = 25.0,
          .max_iterations = 1024,
          .tolerance = 1e-12,
      });

  if (!Check(low_lambda.size() == src.size(), "Tikhonov low-lambda size mismatch")) {
    return false;
  }
  if (!Check(high_lambda.size() == src.size(), "Tikhonov high-lambda size mismatch")) {
    return false;
  }
  if (!Check(IsNonDecreasing(low_lambda), "Tikhonov low-lambda must be non-decreasing")) {
    return false;
  }
  if (!Check(IsNonDecreasing(high_lambda), "Tikhonov high-lambda must be non-decreasing")) {
    return false;
  }

  const double rmse_low = Rmse(src, low_lambda);
  const double rmse_high = Rmse(src, high_lambda);
  if (!Check(rmse_high > rmse_low,
             "Tikhonov: stronger penalty must increase approximation error")) {
    return false;
  }

  const double curv_low = CurvatureEnergy(low_lambda);
  const double curv_high = CurvatureEnergy(high_lambda);
  if (!Check(curv_high < curv_low,
             "Tikhonov: stronger penalty must reduce curvature energy")) {
    return false;
  }

  return true;
}

bool TestApplyRegularizationRebuildsCurves() {
  const std::vector<BookLevel> asks = {
      Level(70000, 1000),
      Level(70010, 1000),
      Level(70020, 1000),
      Level(70030, 1000),
  };

  const auto base = BuildDepthSideCurvesFromLevels(asks, ExecutionSide::kBuy);
  const auto moreau = ApplyMoreauRegularizationL2(
      base,
      MoreauL2RegularizationConfig{.smoothing = 0.8});
  const auto tikhonov = ApplyTikhonovRegularizationL2(
      base,
      TikhonovL2RegularizationConfig{.curvature_penalty = 3.0});

  if (!Check(moreau.s_of_q.size() == base.s_of_q.size(), "apply moreau: S(q) size mismatch")) {
    return false;
  }
  if (!Check(moreau.p_of_q.size() == base.p_of_q.size(), "apply moreau: p(q) size mismatch")) {
    return false;
  }
  if (!Check(!moreau.l_of_v_monotone.empty(), "apply moreau: monotone L(v) must exist")) {
    return false;
  }

  if (!Check(tikhonov.s_of_q.size() == base.s_of_q.size(),
             "apply tikhonov: S(q) size mismatch")) {
    return false;
  }
  if (!Check(tikhonov.p_of_q.size() == base.p_of_q.size(),
             "apply tikhonov: p(q) size mismatch")) {
    return false;
  }
  if (!Check(!tikhonov.l_of_v_monotone.empty(),
             "apply tikhonov: monotone L(v) must exist")) {
    return false;
  }

  for (std::size_t i = 1; i < moreau.p_of_q.size(); ++i) {
    if (!Check(moreau.p_of_q[i].price.units >= moreau.p_of_q[i - 1].price.units,
               "apply moreau: buy p(q) must stay non-decreasing")) {
      return false;
    }
  }
  for (std::size_t i = 1; i < tikhonov.p_of_q.size(); ++i) {
    if (!Check(tikhonov.p_of_q[i].price.units >= tikhonov.p_of_q[i - 1].price.units,
               "apply tikhonov: buy p(q) must stay non-decreasing")) {
      return false;
    }
  }

  return true;
}

}  // namespace

int main() {
  if (!TestMoreauL2ParameterEffect()) return EXIT_FAILURE;
  if (!TestTikhonovL2ParameterEffect()) return EXIT_FAILURE;
  if (!TestApplyRegularizationRebuildsCurves()) return EXIT_FAILURE;

  std::cout << "[OK] depth_curve_regularization_test passed" << std::endl;
  return EXIT_SUCCESS;
}
