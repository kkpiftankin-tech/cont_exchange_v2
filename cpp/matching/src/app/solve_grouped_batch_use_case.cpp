// ============================================================================
// solve_grouped_batch_use_case.cpp — F-09 (T-F09-045). См. .hpp.
// ============================================================================

#include "app/solve_grouped_batch_use_case.hpp"

#include <utility>

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
    results.push_back(std::move(result));
  }

  return results;
}

}  // namespace cex::matching::app
