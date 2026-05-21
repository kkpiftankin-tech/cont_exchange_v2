#include "infra/venue_kafka_consumer.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "fob/venue/v1/venue.pb.h"

namespace cex::backtest::infra {

VenueKafkaConsumer::VenueKafkaConsumer(app::VenueReplayUseCases* uc,
                                       const std::string& brokers)
    : uc_(uc), brokers_(brokers) {}

void VenueKafkaConsumer::start() {
  running_.store(true);
  t_ = std::thread([this] { loop(); });
}

void VenueKafkaConsumer::stop() {
  running_.store(false);
  if (t_.joinable()) t_.join();
}

void VenueKafkaConsumer::loop() {
  cex::common::KafkaConsumer consumer({
      .brokers = brokers_,
      .group_id = "backtest-venue",
      .client_id = "backtest",
      .enable_auto_commit = false,
  });
  consumer.subscribe({"venue.snapshots", "venue.liquidity.fob"});

  cex::common::log_json("INFO", "Backtest venue Kafka consumer started",
                        {{"topics", "venue.snapshots,venue.liquidity.fob"},
                         {"group_id", "backtest-venue"}});

  while (running_.load()) {
    bool ok = consumer.poll_once(500, [this](const std::string& topic,
                                             const std::string& key,
                                             const std::string& payload) {
      (void)key;
      if (topic == "venue.snapshots") {
        fob::venue::v1::VenueSnapshot snapshot;
        if (!cex::common::from_bytes(payload, snapshot)) {
          cex::common::log_json("ERROR", "Backtest: failed to parse VenueSnapshot");
          return;
        }
        uc_->OnVenueSnapshot(snapshot);
      } else if (topic == "venue.liquidity.fob") {
        fob::venue::v1::VenueLiquidityCurve curve;
        if (!cex::common::from_bytes(payload, curve)) {
          cex::common::log_json("ERROR", "Backtest: failed to parse VenueLiquidityCurve");
          return;
        }
        uc_->OnVenueLiquidityCurve(curve);
      }
    });

    if (!ok) break;
  }

  cex::common::log_json("INFO", "Backtest venue Kafka consumer stopped");
}

}  // namespace cex::backtest::infra
