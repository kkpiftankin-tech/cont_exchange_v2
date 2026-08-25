#pragma once

#include "cex/common/decimal.hpp"
#include "cex/common/kafka.hpp"

#include "domain/ports/i_health_state_publisher.hpp"

namespace cex::venue_health::infra {

class KafkaProducer final : public domain::IHealthStatePublisher {
 public:
  explicit KafkaProducer(std::string brokers);

  bool Publish(const domain::VenueState&) override;

 private:
  common::KafkaProducer producer_;
};

}  // namespace cex::venue_health::infra