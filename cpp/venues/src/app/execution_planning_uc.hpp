#pragma once

#include "fob/execution/v1/execution.pb.h"

namespace cex::matching::app {

class IExecutionPlanningUseCases {
 public:
  virtual ~IExecutionPlanningUseCases() = default;

  /**
   * Основной входной метод для F12-PLAN-1.
   * Здесь должна происходить:
   * - Проверка идемпотентности по intent.hedge_flow_id()
   * - Запуск алгоритма multi-venue routing (F12-PLAN-4)
   */
  virtual void OnExecutionIntent(const fob::execution::v1::ExecutionIntent& intent) = 0;
};

} // namespace cex::matching::app