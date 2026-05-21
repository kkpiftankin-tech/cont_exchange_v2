#include "kafka_consumer.hpp"

#include "cex/common/kafka.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "infra/mappers/venue_health.hpp"
#include "infra/mappers/venue_liquidity_curve.hpp"

namespace cex::observability::infra {

KafkaConsumer::KafkaConsumer(std::string brokers, app::Service &service)
    : brokers_(std::move(brokers)), service_(service) {}

void KafkaConsumer::start() {
  running_.store(true);
  t_ = std::thread([this]() { loop(); });
}

void KafkaConsumer::stop() {
  running_.store(false);
  t_.join();
}

void KafkaConsumer::loop() {
  common::KafkaConsumer consumer({
      .brokers = brokers_,
      .group_id = "observability",
      .client_id = "observability",
      .enable_auto_commit = false,
  });
  consumer.subscribe({"venue.health", "venue.liquidity.fob"});

  auto handler = [this](const std::string &topic, const std::string &, const std::string &payload) {
    if (topic == "venue.health") {
      fob::venue::v1::VenueHealth report;
      if (!common::from_bytes(payload, report)) {
        common::log_json("ERROR", "Failed to parse VenueHealth");
        return;
      }
      if (report.event_type() == fob::venue::v1::VENUE_HEALTH_EVENT_TYPE_AGGREGATED) {
        service_.OnVenueHealth(mappers::FromProto(report));
      }
      return;
    }

    if (topic == "venue.liquidity.fob") {
      fob::venue::v1::VenueLiquidityCurve curve;
      if (!common::from_bytes(payload, curve)) {
        common::log_json("ERROR", "Failed to parse VenueLiquidityCurve");
        return;
      }
      service_.OnVenueLiquidityCurve(mappers::FromProto(curve));
      return;
    }

    throw std::logic_error("unknown topic");
  };

  const int timeout_ms = 500;

  while (running_.load()) {
    if (bool ok = consumer.poll_once(timeout_ms, handler); !ok) {
      break;
    }
  }
}

}  // namespace cex::observability::infra
