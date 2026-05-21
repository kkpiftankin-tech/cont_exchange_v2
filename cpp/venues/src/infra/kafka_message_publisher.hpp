#pragma once

#include "app/snapshot_producer.hpp"
#include "cex/common/kafka.hpp"

namespace cex::venues::infra {

// Adapter: wraps KafkaProducer behind IMessagePublisher interface.
class KafkaMessagePublisher final : public app::IMessagePublisher {
 public:
  explicit KafkaMessagePublisher(cex::common::KafkaProducer* producer)
      : producer_(producer) {}

  bool Publish(const std::string& topic,
               const std::string& key,
               const std::string& payload) override {
    if (producer_ == nullptr) return false;
    return producer_->produce(topic, key, payload);
  }

 private:
  cex::common::KafkaProducer* producer_;
};

}  // namespace cex::venues::infra
