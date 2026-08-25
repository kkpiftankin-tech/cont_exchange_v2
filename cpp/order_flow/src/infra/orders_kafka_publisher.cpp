#include "infra/orders_kafka_publisher.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"

namespace cex::order_flow::infra {

OrdersKafkaPublisher::OrdersKafkaPublisher(cex::common::KafkaProducer producer)
    : producer_(std::move(producer)) {}

bool OrdersKafkaPublisher::publish(const fob::orders::v1::OrdersNormalized& event) {
  const std::string topic = "orders.normalized";
  const std::string key = event.meta().partition_key().empty()
                              ? event.meta().correlation_id()
                              : event.meta().partition_key();
  const std::string payload = cex::common::to_bytes(event);
  bool ok = producer_.produce(topic, key, payload);
  if (ok) {
    cex::common::log_json("INFO", "Published OrdersNormalized", {{"topic", topic}, {"key", key}});
  }
  return ok;
}

}  // namespace cex::order_flow::infra
