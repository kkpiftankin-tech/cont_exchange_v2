#pragma once
// ============================================================================
// solve_grouped_batch_use_case.hpp — F-09 (T-F09-045). Matching app.
//
// Оркестрация одного grouped batch-цикла: для каждой активной группы считает
// feasible caps и запускает grouped solver, собирая результат на группу.
// Детерминирована относительно входа (AC-F09-010) → replay даёт тот же выход.
//
// Загрузка активных групп из репозитория (T-F09-047) и публикация ExecutionGroup
// в Kafka (T-F09-046) — отдельные адаптеры; этот use case — чистое ядро solve.
// Вызывается в том же batch-цикле после single-leg F-04 solving (не ломая F-04).
// ============================================================================

#include <string>
#include <vector>

#include "domain/grouped_solver.hpp"
#include "domain/multileg_feasible_caps.hpp"
#include "domain/multileg_vector_order.hpp"

namespace cex::matching::app {

/// Результат solve по одной группе.
struct GroupBatchResult {
  std::string parent_order_id;
  domain::GroupedSolveResult solve;
};

class SolveGroupedBatchUseCase {
 public:
  explicit SolveGroupedBatchUseCase(domain::IGroupedSolver& solver);

  /// Решает batch-цикл для набора активных групп. Порядок результатов = порядку
  /// входных групп. Чистая функция относительно (groups, reference_prices).
  [[nodiscard]] std::vector<GroupBatchResult> Execute(
      const std::vector<domain::MultiLegVectorOrder>& groups,
      const domain::ReferencePrices& reference_prices);

 private:
  domain::IGroupedSolver& solver_;
};

}  // namespace cex::matching::app
