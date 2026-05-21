#include "kafka_consumer.hpp"

#include "fob/venue/v1/venue.grpc.pb.h"

#include "cex/common/kafka.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"

#include "../mappers/raw_report.hpp"

namespace cex::venue_health::infra {

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
  cex::common::KafkaConsumer consumer({
      .brokers = brokers_,
      .group_id = "venue_health",
      .client_id = "venue_health",
      .enable_auto_commit = false,
  });
  consumer.subscribe({"venue.health"});

  const int timeout_ms = 500;

  auto handler = [this](const std::string &, const std::string &,
                        const std::string &payload) {
    fob::venue::v1::VenueHealth report;
    if (!common::from_bytes(payload, report)) {
      common::log_json("ERROR", "Failed to parse VenueHealth");
      return;
    }

    auto type = report.event_type();
    switch (type) {
    case fob::venue::v1::VENUE_HEALTH_EVENT_TYPE_RAW: {
      common::log_json("INFO", "Aggregating venue health event",
                       {{"service", "venue_health"},
                        {"component", "venue_health_consumer"},
                        {"participant", "Venue Health & Routing Service"},
                        {"stage", "consume_raw_health"},
                        {"topic", "venue.health"},
                        {"venue", report.venue()},
                        {"source_file",
                         "cpp/venue_health/src/infra/kafka/kafka_consumer.cpp"}});
      service_.OnRawReport(mappers::FromProto(report));
      return;
    }

    case fob::venue::v1::VENUE_HEALTH_EVENT_TYPE_AGGREGATED: {
      // We already aggregaed, do nothing
      common::log_json("INFO",
                       "Skipping already aggregated venue health event",
                       {{"service", "venue_health"},
                        {"component", "venue_health_consumer"},
                        {"participant", "Venue Health & Routing Service"},
                        {"stage", "skip_aggregated_health"},
                        {"topic", "venue.health"},
                        {"venue", report.venue()},
                        {"source_file",
                         "cpp/venue_health/src/infra/kafka/kafka_consumer.cpp"}});
      return;
    }

    default: {
      common::log_json("WARN",
                       "unknown enent type:" +
                           fob::venue::v1::VenueHealthEventType_Name(type));
      return;
    }
    }
  };

  while (running_.load()) {
    if (bool ok = consumer.poll_once(timeout_ms, handler); !ok) {
      break;
    }
  }
}

} // namespace cex::venue_health::infra
