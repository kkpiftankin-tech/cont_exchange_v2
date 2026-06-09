// ============================================================================
// combo_policy.cpp — F-09 (T-F09-002). См. .hpp.
// ============================================================================

#include "domain/combo_policy.hpp"

#include <algorithm>

namespace cex::order_flow::domain {

ComboPolicy ComboPolicy::Permissive() {
  ComboPolicy p;
  p.grouped_orders_enabled = true;
  p.multileg_vector_solver_enabled = true;
  p.external_compensating_enabled = true;
  p.max_legs_per_group = 1000;
  p.max_children_per_graph = 1000;
  p.allowed_combo_types.clear();        // всё разрешено
  p.allowed_atomicity_policies.clear(); // всё разрешено
  return p;
}

std::optional<DomainError> ComboPolicy::CheckCreate(const ComboOrder& combo) const {
  if (!grouped_orders_enabled) {
    return DomainError{"COMBO_DISABLED", "grouped/combo orders are disabled by policy"};
  }

  if (combo.execution_mode == ExecutionMode::kMultilegVectorSolver &&
      !multileg_vector_solver_enabled) {
    return DomainError{"COMBO_MULTILEG_DISABLED",
                       "multileg_vector_solver mode is disabled by policy"};
  }

  if (combo.atomicity_scope == AtomicityScope::kExternalCompensating &&
      !external_compensating_enabled) {
    return DomainError{"COMBO_EXTERNAL_COMP_DISABLED",
                       "external_compensating scope is disabled by policy"};
  }

  if (static_cast<int>(combo.legs.size()) > max_legs_per_group) {
    return DomainError{"COMBO_TOO_MANY_LEGS", "legs exceed maxLegsPerGroup"};
  }

  if (!allowed_combo_types.empty() &&
      std::find(allowed_combo_types.begin(), allowed_combo_types.end(),
                combo.combo_type) == allowed_combo_types.end()) {
    return DomainError{"COMBO_TYPE_NOT_ALLOWED", "combo_type not in allowedComboTypes"};
  }

  if (!allowed_atomicity_policies.empty() &&
      std::find(allowed_atomicity_policies.begin(), allowed_atomicity_policies.end(),
                combo.atomicity_policy) == allowed_atomicity_policies.end()) {
    return DomainError{"COMBO_POLICY_NOT_ALLOWED",
                       "atomicity_policy not in allowedAtomicityPolicies"};
  }

  return std::nullopt;
}

}  // namespace cex::order_flow::domain
