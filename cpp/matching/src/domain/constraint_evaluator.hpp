#pragma once
// ============================================================================
// constraint_evaluator.hpp — F-09 (T-F09-094, MVP-3). Matching domain (pure).
//
// Оценивает групповые ограничения combo (spread/factor/notional) против solved
// leg execs + reference prices. Возвращает нарушенные (hard блокируют группу,
// soft → degrade). Чистая детерминированная функция (без QP — feasibility-check;
// полный re-solve A_g e = b_g α через QP-солвер — отдельный шаг MVP-3).
//   spread_range      : Σ coeff·price ∈ [L,U]
//   factor_neutrality : Σ coeff·signed_exec ∈ [L,U]
//   max_total_notional: Σ |exec·price| ≤ U
// ============================================================================

#include <string>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/grouped_solver.hpp"          // LegExec
#include "domain/multileg_feasible_caps.hpp"  // ReferencePrices
#include "domain/multileg_vector_order.hpp"   // GroupConstraint

namespace cex::matching::domain {

struct ConstraintViolation {
  std::string constraint_id;
  GroupConstraintType type{GroupConstraintType::kOther};
  cex::common::Decimal value{};  ///< вычисленное значение комбинации
  bool hard{true};               ///< true = блокирует группу (severity=hard)
};

/// Возвращает нарушенные ограничения. Детерминирована относительно входа.
[[nodiscard]] std::vector<ConstraintViolation> EvaluateConstraints(
    const std::vector<GroupConstraint>& constraints,
    const std::vector<LegExec>& leg_execs, const ReferencePrices& prices);

}  // namespace cex::matching::domain
