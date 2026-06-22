#pragma once
// ============================================================================
// combo_policy.hpp — F-09 (T-F09-002). Order_flow domain (pure).
//
// Feature flags + grouped policy для combo-заявок (документ F-09 v2 §6, §3.9).
// Честный create-side gate: combo, нарушающая флаги/лимиты, отклоняется на
// приёме (order_flow), а не «тихо» исполняется. Pure (без env/IO) — env-загрузка
// делается в main через LoadComboPolicyFromEnv (infra-слой).
// ============================================================================

#include <cstdint>
#include <optional>
#include <vector>

#include "domain/combo_order.hpp"

namespace cex::order_flow::domain {

struct ComboPolicy {
  // Feature flags.
  bool grouped_orders_enabled{true};
  bool multileg_vector_solver_enabled{true};
  bool external_compensating_enabled{false};

  // Лимиты группы.
  int max_legs_per_group{8};
  int max_children_per_graph{16};

  // Пустой список = разрешено всё. Непустой = whitelist.
  std::vector<ComboType> allowed_combo_types;
  std::vector<AtomicityPolicy> allowed_atomicity_policies;

  // Допуски (используются risk/solver в поздних фазах; здесь — переносятся).
  std::int32_t ratio_tolerance_bps{50};
  std::int32_t max_weight_deviation_bps{50};
  std::int32_t max_grouped_solve_time_ms{100};
  bool require_preview_before_submit{false};

  /// Разрешающая политика по умолчанию (всё включено, без whitelist).
  /// Используется в тестах и как fallback.
  static ComboPolicy Permissive();

  /// Проверяет combo против политики на приёме. nullopt = принято.
  [[nodiscard]] std::optional<DomainError> CheckCreate(const ComboOrder& combo) const;
};

}  // namespace cex::order_flow::domain
