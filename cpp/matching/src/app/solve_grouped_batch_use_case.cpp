// ============================================================================
// solve_grouped_batch_use_case.cpp — F-09 (T-F09-045). См. .hpp.
// ============================================================================

#include "app/solve_grouped_batch_use_case.hpp"

#include <utility>

#include "cex/common/decimal.hpp"
#include "domain/constraint_evaluator.hpp"

namespace cex::matching::app {

SolveGroupedBatchUseCase::SolveGroupedBatchUseCase(domain::IGroupedSolver& solver)
    : solver_(solver) {}

std::vector<GroupBatchResult> SolveGroupedBatchUseCase::Execute(
    const std::vector<domain::MultiLegVectorOrder>& groups,
    const domain::ReferencePrices& reference_prices) {
  std::vector<GroupBatchResult> results;
  results.reserve(groups.size());

  for (const auto& group : groups) {
    domain::GroupedSolveInput input;
    input.order = group;
    // feasible caps по ногам, затем grouped solve с учётом atomicity policy.
    input.feasible_caps = domain::ComputeFeasibleCaps(group, reference_prices);
    input.reference_prices = reference_prices;

    GroupBatchResult result;
    result.parent_order_id = group.parent_order_id;
    result.solve = solver_.Solve(input);

    // MVP-3: применяем групповые ограничения к solved execs. Hard-нарушение
    // блокирует группу (ничего не исполняется), soft → degrade (фиксируем).
    if (!group.constraints.empty() && !result.solve.leg_execs.empty()) {
      const auto violations =
          domain::EvaluateConstraints(group.constraints, result.solve.leg_execs, reference_prices);
      bool hard = false;
      for (const auto& v : violations) {
        result.solve.violated_constraints.push_back(v.constraint_id);
        if (v.hard) hard = true;
      }
      if (hard) {
        result.solve.leg_execs.clear();
        result.solve.execution_scale = cex::common::Decimal::zero();
        result.solve.status = domain::GroupExecStatus::kBlocked;
      } else if (!violations.empty()) {
        result.solve.status = domain::GroupExecStatus::kDegraded;
      }
    }

    results.push_back(std::move(result));
  }

  return results;
}

}  // namespace cex::matching::app
