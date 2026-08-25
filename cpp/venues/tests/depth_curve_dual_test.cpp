#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "domain/depth_curve_builder.hpp"

namespace {

using cex::common::Decimal;
using cex::venues::domain::ApplyFenchelLegendreLayer;
using cex::venues::domain::BookLevel;
using cex::venues::domain::BuildDepthSideCurvesFromLevels;
using cex::venues::domain::BuildFenchelLegendreLayer;
using cex::venues::domain::ExecutionSide;
using cex::venues::domain::FenchelLegendreConfig;
using cex::venues::domain::FenchelLegendreLayer;
using cex::venues::domain::SOfQPoint;

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

bool CheckApprox(const double actual,
                 const double expected,
                 const double tolerance,
                 const std::string& field_name) {
  if (std::fabs(actual - expected) <= tolerance) return true;
  std::cerr << "[FAIL] " << field_name << " expected " << expected
            << " got " << actual << std::endl;
  return false;
}

bool TestFenchelLegendreClosedFormExample() {
  // Convex piecewise-linear S(q):
  // q: 0,1,2,3
  // S: 0,10,30,60
  const std::vector<SOfQPoint> s_of_q = {
      CostPoint(0, 0),
      CostPoint(1, 10),
      CostPoint(2, 30),
      CostPoint(3, 60),
  };

  FenchelLegendreConfig cfg;
  cfg.p_min = Decimal{.units = 5, .scale = 0};
  cfg.p_max = Decimal{.units = 35, .scale = 0};
  cfg.p_step = Decimal{.units = 5, .scale = 0};
  cfg.include_price_knots = false;

  const FenchelLegendreLayer layer = BuildFenchelLegendreLayer(s_of_q, cfg);
  if (!Check(layer.s_star_of_p.size() == 7, "S*(p) grid size mismatch")) return false;
  if (!Check(layer.q_star_of_p.size() == 7, "q*(p) grid size mismatch")) return false;

  const std::vector<double> expected_p = {5, 10, 15, 20, 25, 30, 35};
  const std::vector<double> expected_s_star = {0, 0, 5, 10, 20, 30, 45};
  const std::vector<double> expected_q_star = {0, 1, 1, 2, 2, 3, 3};

  for (std::size_t i = 0; i < expected_p.size(); ++i) {
    if (!CheckApprox(static_cast<double>(layer.s_star_of_p[i].price), expected_p[i], 1e-6,
                     "S*(p) price grid")) {
      return false;
    }
    if (!CheckApprox(static_cast<double>(layer.s_star_of_p[i].dual_value), expected_s_star[i],
                     1e-6,
                     "S*(p) values")) {
      return false;
    }
    if (!CheckApprox(static_cast<double>(layer.q_star_of_p[i].optimal_qty), expected_q_star[i],
                     1e-6,
                     "q*(p) values")) {
      return false;
    }
  }

  return true;
}

bool TestFenchelInequalityOnGrid() {
  const std::vector<SOfQPoint> s_of_q = {
      CostPoint(0, 0),
      CostPoint(1, 10),
      CostPoint(2, 30),
      CostPoint(3, 60),
  };

  FenchelLegendreConfig cfg;
  cfg.p_min = Decimal{.units = 5, .scale = 0};
  cfg.p_max = Decimal{.units = 35, .scale = 0};
  cfg.p_step = Decimal{.units = 5, .scale = 0};

  const FenchelLegendreLayer layer = BuildFenchelLegendreLayer(s_of_q, cfg);
  if (layer.empty()) return false;

  for (std::size_t i = 0; i < layer.s_star_of_p.size(); ++i) {
    const double p = static_cast<double>(layer.s_star_of_p[i].price);
    const double s_star = static_cast<double>(layer.s_star_of_p[i].dual_value);

    for (const auto& sq : s_of_q) {
      const double q = static_cast<double>(sq.qty);
      const double s = static_cast<double>(sq.cumulative_cost);
      const double lhs = s + s_star;
      const double rhs = p * q;
      if (!Check(lhs + 1e-6 >= rhs, "Fenchel inequality S(q)+S*(p)>=p*q violated")) {
        return false;
      }
    }
  }

  return true;
}

bool TestApplyFenchelLegendreLayerStoresDualTables() {
  const std::vector<BookLevel> asks = {
      Level(70000, 1000),
      Level(70010, 1000),
      Level(70030, 1000),
      Level(70060, 1000),
  };

  const auto base = BuildDepthSideCurvesFromLevels(asks, ExecutionSide::kBuy);
  const auto with_dual = ApplyFenchelLegendreLayer(base);

  if (!Check(!with_dual.fenchel_legendre.empty(), "dual layer must not be empty")) {
    return false;
  }

  if (!Check(with_dual.fenchel_legendre.s_star_of_p.size() ==
             with_dual.fenchel_legendre.q_star_of_p.size(),
             "dual tables must have same size")) {
    return false;
  }

  for (std::size_t i = 1; i < with_dual.fenchel_legendre.q_star_of_p.size(); ++i) {
    if (!Check(with_dual.fenchel_legendre.q_star_of_p[i].optimal_qty.units >=
               with_dual.fenchel_legendre.q_star_of_p[i - 1].optimal_qty.units,
               "q*(p) must be non-decreasing over p-grid")) {
      return false;
    }
  }

  // Transform should not mutate original primal tables.
  if (!Check(with_dual.p_of_q.size() == base.p_of_q.size(), "p(q) size must stay same")) {
    return false;
  }
  if (!Check(with_dual.s_of_q.size() == base.s_of_q.size(), "S(q) size must stay same")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  if (!TestFenchelLegendreClosedFormExample()) return EXIT_FAILURE;
  if (!TestFenchelInequalityOnGrid()) return EXIT_FAILURE;
  if (!TestApplyFenchelLegendreLayerStoresDualTables()) return EXIT_FAILURE;

  std::cout << "[OK] depth_curve_dual_test passed" << std::endl;
  return EXIT_SUCCESS;
}
