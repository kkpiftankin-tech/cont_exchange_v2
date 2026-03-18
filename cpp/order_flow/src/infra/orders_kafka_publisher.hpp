#pragma once
#include <string>

#include "cex/common/kafka.hpp"
#include "fob/orders/v1/orders.pb.h"

namespace cex::order_flow::infra {

class OrdersKafkaPublisher {
 public:
  explicit OrdersKafkaPublisher(cex::common::KafkaProducer producer);

  bool publish(const fob::orders::v1::OrdersNormalized& event);

 private:
  cex::common::KafkaProducer producer_;
};

}  // namespace cex::order_flow::infra
