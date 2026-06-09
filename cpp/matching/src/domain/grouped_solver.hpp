#pragma once
// ============================================================================
// grouped_solver.hpp — F-09 (T-F09-044). Matching domain.
//
// Интерфейс grouped vector solver + структуры входа/выхода. Solver выбирает
// масштаб группы и распределяет исполнение по ногам согласно target ratio и
// atomicity policy. Алгоритм за интерфейсом (обратимо: bisection→QP в MVP-3).
// ============================================================================

#include <string>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/multileg_feasible_caps.hpp"
#include "domain/multileg_vector_order.hpp"

namespace cex::matching::domain {

/// Исполнение одной ноги в результате solve.
struct LegExec {
  std::string instrument_symbol;
  cex::common::Decimal executed_qty{};  ///< base qty, >= 0
  cex::common::Decimal target_ratio{};  ///< знаковый ρ (для side/знака downstream)
};

/// Итоговый статус исполнения группы.
enum class GroupExecStatus {
  kFullyExecuted,  ///< strict_atomic: вся группа исполнена (α = 1)
  kScaled,         ///< scalable_atomic: исполнена доля α ∈ (0,1)
  kBlocked,        ///< не исполнено (α = 0): strict не атомарен / scalable < α_min
  kDegraded        ///< best_effort: исполнено с нарушением соотношений
};

struct GroupedSolveInput {
  MultiLegVectorOrder order;
  std::vector<FeasibleCap> feasible_caps;  ///< из ComputeFeasibleCaps (T-F09-043)
  ReferencePrices reference_prices;        ///< для диагностики
};

struct GroupedSolveResult {
  cex::common::Decimal execution_scale{};       ///< α ∈ [0,1]
  std::vector<LegExec> leg_execs;               ///< пустой при α = 0
  std::vector<std::string> violated_constraints;
  std::string fallback_action{"none"};          ///< из fallback_policy при блокировке
  GroupExecStatus status{GroupExecStatus::kBlocked};
  std::string binding_leg;                       ///< диагностика: связывающая нога
};

/// Интерфейс grouped solver. Детерминирован относительно входа (AC-F09-010).
struct IGroupedSolver {
  virtual ~IGroupedSolver() = default;
  [[nodiscard]] virtual GroupedSolveResult Solve(const GroupedSolveInput& input) = 0;
};

}  // namespace cex::matching::domain
