#pragma once
// ============================================================================
// cancel_combo_order_use_case.hpp — F-09 (T-F09-033).
//
// Отмена ComboOrder: идемпотентно переводит combo + активные ноги в cancelled
// и публикует FlowOrderCancel для дочерних FlowOrder (matching снимет их).
// Повторный cancel уже-терминальной заявки → success без side-effects (AC-F09-007
// idempotency). OCO sibling-on-fill — задача matching (child graph, MVP-4);
// здесь — отмена всей группы по запросу пользователя.
// ============================================================================

#include "fob/orders/v1/combo.pb.h"
#include "infra/orders_normalized_grouped_producer.hpp"
#include "infra/postgres_combo_order_repository.hpp"

namespace cex::order_flow::app {

class CancelComboOrderUseCase {
 public:
  CancelComboOrderUseCase(infra::IComboOrderRepository& repository,
                          infra::OrdersNormalizedGroupedProducer& producer);

  fob::orders::v1::CancelComboOrderResponse Execute(
      const fob::orders::v1::CancelComboOrderRequest& req);

 private:
  infra::IComboOrderRepository& repository_;
  infra::OrdersNormalizedGroupedProducer& producer_;
};

}  // namespace cex::order_flow::app
