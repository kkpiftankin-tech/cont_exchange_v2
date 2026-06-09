#pragma once
// ============================================================================
// grouped_solver_bisection.hpp — F-09 (T-F09-044). MVP-2 реализация IGroupedSolver.
//
// Для ratio/basket задача линейна по масштабу группы, поэтому «bisection»
// вырождается в closed-form: G = min_i (Q_i / ρ_i). Истинная бисекция
// (нелинейные spread/factor ограничения A_g e = b_g α) — MVP-3 (QP, Eigen/OSQP).
// ============================================================================

#include "domain/grouped_solver.hpp"

namespace cex::matching::domain {

class GroupedSolverBisection final : public IGroupedSolver {
 public:
  [[nodiscard]] GroupedSolveResult Solve(const GroupedSolveInput& input) override;
};

}  // namespace cex::matching::domain
