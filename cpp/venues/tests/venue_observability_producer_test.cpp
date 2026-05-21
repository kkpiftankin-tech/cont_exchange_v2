#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "cex/common/proto.hpp"
#include "cex/common/time.hpp"
#include "domain/venue_adapter.hpp"
#include "fob/observability/v1/observability.pb.h"
#include "fob/venue/v1/venue.pb.h"
#include "infra/venue_observability_producer.hpp"

namespace {

using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueHeartbeat;
using cex::venues::domain::VenueType;
using cex::venues::infra::VenueObservabilityProducer;

struct ProducedRecord {
  std::string topic;
  std::string key;
  std::string payload;
};

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool TestPublishStatusProducesHealthAndMetrics() {
  std::vector<ProducedRecord> records;
  VenueObservabilityProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back(ProducedRecord{topic, key, payload});
        return true;
      });

  VenueHeartbeat hb;
  hb.venue_id = "binance";
  hb.venue_type = VenueType::kCex;
  hb.status = VenueConnectionStatus::kStale;
  hb.timestamp = cex::common::now_ts();
  hb.reconnect_attempts = 3;
  hb.consecutive_errors = 2;
  hb.last_sequence = 12345;
  hb.connect_attempts = 10;
  hb.connect_successes = 8;
  hb.reconnect_calls = 4;
  hb.reconnect_successes = 3;
  hb.connect_success_rate = 0.8;
  hb.reconnect_success_rate = 0.75;
  hb.circuit_breaker_state = "OPEN";
  hb.circuit_breaker_reason = "error_burst";
  hb.circuit_breaker_error_count = 7;

  if (!Check(producer.PublishStatus(hb, "heartbeat_poll"), "PublishStatus should succeed")) {
    return false;
  }
  if (!Check(records.size() == 3,
             "PublishStatus should emit 1 health event and 2 metrics")) {
    return false;
  }

  fob::venue::v1::VenueHealth status_evt;
  if (!Check(cex::common::from_bytes(records[0].payload, status_evt),
             "Health payload must parse as VenueHealth")) {
    return false;
  }
  if (!Check(records[0].topic == "venue.health", "Status event topic must be venue.health")) {
    return false;
  }
  if (!Check(status_evt.venue() == "binance", "VenueHealth venue mismatch")) {
    return false;
  }
  if (!Check(status_evt.event_type() == fob::venue::v1::VENUE_HEALTH_EVENT_TYPE_RAW,
             "VenueHealth event type mismatch")) {
    return false;
  }
  if (!Check(status_evt.status() == fob::venue::v1::VENUE_HEALTH_STATUS_STALE,
             "Stale status must map to VenueHealth STALE")) {
    return false;
  }
  if (!Check(status_evt.breaker_state() == fob::venue::v1::CIRCUIT_BREAKER_STATE_OPEN,
             "Circuit breaker state must be published")) {
    return false;
  }
  if (!Check(status_evt.error_count() == 7,
             "Circuit breaker error count must be published")) {
    return false;
  }
  if (!Check(status_evt.routing_recommendation() ==
                 fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK,
             "Open breaker must block routing")) {
    return false;
  }

  fob::observability::v1::MetricPoint metric_connect;
  if (!Check(cex::common::from_bytes(records[1].payload, metric_connect),
             "First metric payload parse failed")) {
    return false;
  }
  if (!Check(records[1].topic == "metrics", "First metric topic must be metrics")) return false;
  if (!Check(metric_connect.name() == "venue.connect.success_rate",
             "First metric name mismatch")) {
    return false;
  }

  fob::observability::v1::MetricPoint metric_reconnect;
  if (!Check(cex::common::from_bytes(records[2].payload, metric_reconnect),
             "Second metric payload parse failed")) {
    return false;
  }
  if (!Check(metric_reconnect.name() == "venue.reconnect.success_rate",
             "Second metric name mismatch")) {
    return false;
  }

  return true;
}

bool TestPublishTrafficAndError() {
  std::vector<ProducedRecord> records;
  VenueObservabilityProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back(ProducedRecord{topic, key, payload});
        return true;
      });

  if (!Check(producer.PublishTraffic("binance", "ws.depth", 512, true, "inbound", "depthUpdate"),
             "PublishTraffic should succeed")) {
    return false;
  }
  if (!Check(producer.PublishError(
                 "binance", "Depth parse failed", {{"error_code", "DEPTH_PARSE"}}),
             "PublishError should succeed")) {
    return false;
  }

  if (!Check(records.size() == 2, "Traffic+Error should produce 2 log events")) return false;
  if (!Check(records[0].topic == "logs" && records[1].topic == "logs",
             "Traffic and error should use logs topic")) {
    return false;
  }

  fob::observability::v1::LogEvent traffic_evt;
  if (!Check(cex::common::from_bytes(records[0].payload, traffic_evt),
             "Traffic payload must parse as LogEvent")) {
    return false;
  }
  if (!Check(traffic_evt.message() == "VENUE_TRAFFIC", "Traffic message mismatch")) return false;

  fob::observability::v1::LogEvent error_evt;
  if (!Check(cex::common::from_bytes(records[1].payload, error_evt),
             "Error payload must parse as LogEvent")) {
    return false;
  }
  if (!Check(error_evt.level() == fob::observability::v1::LogEvent_Level_ERROR,
             "Error event level must be ERROR")) {
    return false;
  }
  if (!Check(error_evt.message() == "Depth parse failed", "Error message mismatch")) {
    return false;
  }

  return true;
}

bool TestPublishStatusRoutingModeOverride() {
  std::vector<ProducedRecord> records;
  VenueObservabilityProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back(ProducedRecord{topic, key, payload});
        return true;
      });

  VenueHeartbeat hb;
  hb.venue_id = "coinbase";
  hb.venue_type = VenueType::kCex;
  hb.status = VenueConnectionStatus::kConnected;
  hb.timestamp = cex::common::now_ts();
  hb.connect_success_rate = 1.0;
  hb.reconnect_success_rate = 1.0;
  hb.circuit_breaker_state = "CLOSED";

  if (!Check(producer.PublishStatus(hb, "heartbeat_poll", "watch"),
             "PublishStatus watch override should succeed")) {
    return false;
  }
  if (!Check(records.size() == 3, "Watch override should publish health + 2 metrics")) {
    return false;
  }

  fob::venue::v1::VenueHealth watch_evt;
  if (!Check(cex::common::from_bytes(records[0].payload, watch_evt),
             "Watch payload must parse as VenueHealth")) {
    return false;
  }
  if (!Check(watch_evt.routing_recommendation() ==
                 fob::venue::v1::ROUTING_RECOMMENDATION_CAUTION,
             "Watch override must force CAUTION recommendation")) {
    return false;
  }
  if (!Check(watch_evt.reason().find("routing_mode=watch") != std::string::npos,
             "Watch override should be reflected in reason")) {
    return false;
  }

  records.clear();
  if (!Check(producer.PublishStatus(hb, "heartbeat_poll", "disable"),
             "PublishStatus disable override should succeed")) {
    return false;
  }
  if (!Check(records.size() == 3, "Disable override should publish health + 2 metrics")) {
    return false;
  }

  fob::venue::v1::VenueHealth disable_evt;
  if (!Check(cex::common::from_bytes(records[0].payload, disable_evt),
             "Disable payload must parse as VenueHealth")) {
    return false;
  }
  if (!Check(disable_evt.routing_recommendation() ==
                 fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK,
             "Disable override must force BLOCK recommendation")) {
    return false;
  }
  if (!Check(disable_evt.reason().find("routing_mode=disable") != std::string::npos,
             "Disable override should be reflected in reason")) {
    return false;
  }
  return true;
}

bool TestFailedProducePropagates() {
  VenueObservabilityProducer producer(
      [](const std::string&, const std::string&, const std::string&) {
        return false;
      });

  if (!Check(!producer.PublishMetric("binance", "venue.test.metric", 1.0),
             "PublishMetric must fail when produce callback fails")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestPublishStatusProducesHealthAndMetrics() && ok;
  ok = TestPublishStatusRoutingModeOverride() && ok;
  ok = TestPublishTrafficAndError() && ok;
  ok = TestFailedProducePropagates() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] venue_observability_producer_test" << std::endl;
  return EXIT_SUCCESS;
}
