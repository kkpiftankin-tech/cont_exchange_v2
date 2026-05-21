#include "kafka_producer.hpp"

#include "cex/common/proto.hpp"

#include "infra/mappers/venue_state.hpp"

namespace cex::venue_health::infra {

KafkaProducer::KafkaProducer(std::string brokers)
    : producer_({std::move(brokers), "venue_health"}) {}

bool KafkaProducer::Publish(const domain::VenueState& state) {
  auto dto = mappers::ToProto(state.Venue(), state);
  return producer_.produce("venue.health", state.Venue(), common::to_bytes(dto));
}

}  // namespace cex::venue_health::infra
