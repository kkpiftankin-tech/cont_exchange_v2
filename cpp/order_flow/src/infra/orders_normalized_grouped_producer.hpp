#pragma once
// ============================================================================
// orders_normalized_grouped_producer.hpp — F-09 (T-F09-035).
//
// MVP-1 (orchestration_only): публикует каждую ногу combo как независимый
// FlowOrderCreate в orders.normalized, c partition_key = combo_order_id
// (AC: key = parentOrderId) и группирующими метаданными в FlowOrder.tags
// (combo_id / leg_id / execution_mode / atomicity_policy / combo_type).
//
// Backward-compatible: matching видит обычные FlowOrderCreate (orchestration_only
// — ноги матчатся независимо). Полный grouped-envelope для
// multileg_vector_solver — MVP-2 (ADR-031, §13.1). Для солвер-режима этот
// продьюсер не применяется (см. guard в .cpp).
// ============================================================================

#include <functional>
#include <string>

#include "domain/combo_order.hpp"
#include "fob/orders/v1/orders.pb.h"

namespace cex::order_flow::infra {

class OrdersNormalizedGroupedProducer {
 public:
  /// Callback публикации одного OrdersNormalized (DI — тестируется мок-функцией,
  /// в prod оборачивает OrdersKafkaPublisher::publish).
  using PublishFn = std::function<bool(const fob::orders::v1::OrdersNormalized&)>;

  explicit OrdersNormalizedGroupedProducer(PublishFn publish);

  /// Публикует все ноги orchestration_only-combo. Возвращает true, только если
  /// все ноги успешно опубликованы. Бросает invalid_argument, если
  /// execution_mode != orchestration_only (солвер-режим — отдельный путь MVP-2).
  bool PublishOrchestration(const domain::ComboOrder& combo,
                            const std::string& user_id,
                            const std::string& account_id);

  /// Публикует FlowOrderCancel для дочерней ноги (order_id = leg_id),
  /// partition_key = combo_order_id. Используется CancelComboOrderUseCase.
  bool PublishLegCancel(const std::string& combo_order_id, const std::string& leg_id,
                        const std::string& user_id, const std::string& reason);

 private:
  PublishFn publish_;
};

}  // namespace cex::order_flow::infra
