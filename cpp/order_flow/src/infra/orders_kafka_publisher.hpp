#pragma once
// =============================================================================
// OrdersKafkaPublisher — тонкая обёртка над KafkaProducer, специализированная
// под публикацию событий fob.orders.v1.OrdersNormalized в топик orders.normalized.
// Сюда же удобно добавить логику ретраев/outbox при необходимости.
// =============================================================================

#include <string>

#include "cex/common/kafka.hpp"
#include "fob/orders/v1/orders.pb.h"

namespace cex::order_flow::infra {

class OrdersKafkaPublisher {
 public:
  // Принимает продюсера by-value (move) — он становится владением паблишера.
  explicit OrdersKafkaPublisher(cex::common::KafkaProducer producer);

  // Сериализует event в bytes и публикует в orders.normalized.
  // Возвращает true, если сообщение успешно поставлено в очередь.
  bool publish(const fob::orders::v1::OrdersNormalized& event);

 private:
  cex::common::KafkaProducer producer_;
};

}  // namespace cex::order_flow::infra
