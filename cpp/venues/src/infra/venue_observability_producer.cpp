#include "infra/venue_observability_producer.hpp"

#include <algorithm>
#include <utility>

#include "cex/common/proto.hpp"
#include "cex/common/log.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"
#include "fob/observability/v1/observability.pb.h"
#include "fob/venue/v1/venue.pb.h"

namespace cex::venues::infra {

namespace {

fob::venue::v1::CircuitBreakerState to_breaker_state(const std::string& state) {
  if (state == "CLOSED") return fob::venue::v1::CIRCUIT_BREAKER_STATE_CLOSED;
  if (state == "OPEN") return fob::venue::v1::CIRCUIT_BREAKER_STATE_OPEN;
  if (state == "HALF_OPEN") return fob::venue::v1::CIRCUIT_BREAKER_STATE_HALF_OPEN;
  return fob::venue::v1::CIRCUIT_BREAKER_STATE_UNSPECIFIED;
}

fob::venue::v1::VenueHealthStatus to_health_status(
    const domain::VenueConnectionStatus status) {
  switch (status) {
    case domain::VenueConnectionStatus::kConnected:
      return fob::venue::v1::VENUE_HEALTH_STATUS_OK;
    case domain::VenueConnectionStatus::kStale:
      return fob::venue::v1::VENUE_HEALTH_STATUS_STALE;
    case domain::VenueConnectionStatus::kDisconnected:
      return fob::venue::v1::VENUE_HEALTH_STATUS_DISCONNECTED;
    case domain::VenueConnectionStatus::kEmpty:
      return fob::venue::v1::VENUE_HEALTH_STATUS_DEGRADED;
  }
  return fob::venue::v1::VENUE_HEALTH_STATUS_UNSPECIFIED;
}

double clamp01(const double value) {
  if (value < 0.0) return 0.0;
  if (value > 1.0) return 1.0;
  return value;
}

double status_penalty(const domain::VenueConnectionStatus status) {
  switch (status) {
    case domain::VenueConnectionStatus::kConnected:
      return 0.0;
    case domain::VenueConnectionStatus::kEmpty:
      return 0.20;
    case domain::VenueConnectionStatus::kStale:
      return 0.25;
    case domain::VenueConnectionStatus::kDisconnected:
      return 0.50;
  }
  return 0.50;
}

double breaker_penalty(const fob::venue::v1::CircuitBreakerState state) {
  switch (state) {
    case fob::venue::v1::CIRCUIT_BREAKER_STATE_OPEN:
      return 0.50;
    case fob::venue::v1::CIRCUIT_BREAKER_STATE_HALF_OPEN:
      return 0.25;
    case fob::venue::v1::CIRCUIT_BREAKER_STATE_CLOSED:
    case fob::venue::v1::CIRCUIT_BREAKER_STATE_UNSPECIFIED:
      return 0.0;
    default:
      return 0.0;
  }
}

fob::venue::v1::RoutingRecommendation routing_recommendation(
    const domain::VenueHeartbeat& heartbeat,
    const fob::venue::v1::CircuitBreakerState breaker_state) {
  if (breaker_state == fob::venue::v1::CIRCUIT_BREAKER_STATE_OPEN ||
      heartbeat.status == domain::VenueConnectionStatus::kDisconnected) {
    return fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK;
  }
  if (breaker_state == fob::venue::v1::CIRCUIT_BREAKER_STATE_HALF_OPEN ||
      heartbeat.status == domain::VenueConnectionStatus::kStale) {
    return fob::venue::v1::ROUTING_RECOMMENDATION_AVOID;
  }
  if (heartbeat.consecutive_errors > 0 ||
      heartbeat.status == domain::VenueConnectionStatus::kEmpty) {
    return fob::venue::v1::ROUTING_RECOMMENDATION_CAUTION;
  }
  return fob::venue::v1::ROUTING_RECOMMENDATION_ALLOW;
}

fob::common::v1::EventMeta make_meta(const std::string& key) {
  fob::common::v1::EventMeta meta;
  meta.set_event_id(cex::common::uuid_v4());
  *meta.mutable_ts_event() = cex::common::now_ts();
  meta.set_source("venues");
  meta.set_correlation_id(cex::common::uuid_v4());
  meta.set_partition_key(key);
  return meta;
}

std::map<std::string, std::string> merge_fields(
    const std::map<std::string, std::string>& lhs,
    const std::map<std::string, std::string>& rhs) {
  std::map<std::string, std::string> out = lhs;
  for (const auto& [k, v] : rhs) out[k] = v;
  return out;
}

}  // namespace

VenueObservabilityProducer::VenueObservabilityProducer(
    cex::common::KafkaProducer producer,
    VenueObservabilityTopics topics)
    : producer_(std::make_unique<cex::common::KafkaProducer>(std::move(producer))),
      topics_(std::move(topics)) {}

VenueObservabilityProducer::VenueObservabilityProducer(
    ProduceFn produce_fn,
    VenueObservabilityTopics topics)
    : produce_fn_(std::move(produce_fn)),
      topics_(std::move(topics)) {}

bool VenueObservabilityProducer::PublishStatus(
    const domain::VenueHeartbeat& heartbeat,
    const std::string& reason,
    const std::string& routing_mode_override) {
  fob::venue::v1::VenueHealth event;
  *event.mutable_meta() = make_meta(heartbeat.venue_id);
  event.set_venue(heartbeat.venue_id);
  *event.mutable_timestamp() = heartbeat.timestamp;
  event.set_event_type(fob::venue::v1::VENUE_HEALTH_EVENT_TYPE_RAW);
  const auto breaker_state = to_breaker_state(heartbeat.circuit_breaker_state);
  event.set_breaker_state(breaker_state);
  event.set_status(to_health_status(heartbeat.status));
  event.set_error_count(heartbeat.circuit_breaker_error_count);

  const double attempts = static_cast<double>(
      heartbeat.connect_attempts + heartbeat.reconnect_calls);
  const double error_rate = attempts > 0.0
      ? static_cast<double>(heartbeat.consecutive_errors) / attempts
      : (heartbeat.consecutive_errors > 0 ? 1.0 : 0.0);
  event.set_error_rate(clamp01(error_rate));
  event.set_health_score(clamp01(
      1.0 - status_penalty(heartbeat.status) - breaker_penalty(breaker_state) -
      std::min(0.25, event.error_rate())));
  auto recommendation = routing_recommendation(heartbeat, breaker_state);
  if (routing_mode_override == "watch" &&
      recommendation == fob::venue::v1::ROUTING_RECOMMENDATION_ALLOW) {
    recommendation = fob::venue::v1::ROUTING_RECOMMENDATION_CAUTION;
  }
  if (routing_mode_override == "disable") {
    recommendation = fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK;
  }
  event.set_routing_recommendation(recommendation);
  event.set_reason(heartbeat.circuit_breaker_reason.empty()
                       ? reason
                       : heartbeat.circuit_breaker_reason);
  if (!routing_mode_override.empty()) {
    event.set_reason(event.reason().empty()
                         ? ("routing_mode=" + routing_mode_override)
                         : (event.reason() + ";routing_mode=" + routing_mode_override));
  }

  const std::string health_payload = cex::common::to_bytes(event);
  bool ok = Produce(topics_.venue_health, heartbeat.venue_id, health_payload);
  cex::common::log_json(ok ? "INFO" : "ERROR", "Published venue.health",
                        {{"service", "venues"},
                         {"component", "venue_health_routing"},
                         {"participant", "Venue Health & Routing Service"},
                         {"stage", "publish_health"},
                         {"topic", topics_.venue_health},
                         {"venue", heartbeat.venue_id},
                         {"status", std::to_string(static_cast<int>(event.status()))},
                         {"routing", std::to_string(static_cast<int>(event.routing_recommendation()))},
                         {"health_score", std::to_string(event.health_score())},
                         {"reason", event.reason()},
                         {"source_file",
                          "cpp/venues/src/infra/venue_observability_producer.cpp"}});
  ok = PublishMetric(
           heartbeat.venue_id,
           "venue.connect.success_rate",
           heartbeat.connect_success_rate,
           {{"venue_type", domain::ToString(heartbeat.venue_type)}}) && ok;
  ok = PublishMetric(
           heartbeat.venue_id,
           "venue.reconnect.success_rate",
           heartbeat.reconnect_success_rate,
           {{"venue_type", domain::ToString(heartbeat.venue_type)}}) && ok;
  return ok;
}

bool VenueObservabilityProducer::PublishTraffic(
    const std::string& venue_id,
    const std::string& channel,
    const std::size_t payload_bytes,
    const bool ok,
    const std::string& direction,
    const std::string& detail) {
  fob::observability::v1::LogEvent event;
  *event.mutable_meta() = make_meta(venue_id);
  event.set_level(ok ? fob::observability::v1::LogEvent_Level_INFO
                     : fob::observability::v1::LogEvent_Level_WARN);
  event.set_message("VENUE_TRAFFIC");
  *event.mutable_timestamp() = cex::common::now_ts();

  (*event.mutable_fields())["venue_id"] = venue_id;
  (*event.mutable_fields())["channel"] = channel;
  (*event.mutable_fields())["direction"] = direction;
  (*event.mutable_fields())["payload_bytes"] = std::to_string(payload_bytes);
  (*event.mutable_fields())["ok"] = ok ? "true" : "false";
  if (!detail.empty()) (*event.mutable_fields())["detail"] = detail;

  return Produce(topics_.logs, venue_id, cex::common::to_bytes(event));
}

bool VenueObservabilityProducer::PublishError(
    const std::string& venue_id,
    const std::string& message,
    const std::map<std::string, std::string>& fields) {
  fob::observability::v1::LogEvent event;
  *event.mutable_meta() = make_meta(venue_id);
  event.set_level(fob::observability::v1::LogEvent_Level_ERROR);
  event.set_message(message);
  *event.mutable_timestamp() = cex::common::now_ts();

  const auto merged = merge_fields(fields, {{"venue_id", venue_id}});
  for (const auto& [k, v] : merged) {
    (*event.mutable_fields())[k] = v;
  }

  return Produce(topics_.logs, venue_id, cex::common::to_bytes(event));
}

bool VenueObservabilityProducer::PublishMetric(
    const std::string& venue_id,
    const std::string& name,
    const double value,
    const std::map<std::string, std::string>& labels) {
  fob::observability::v1::MetricPoint metric;
  *metric.mutable_meta() = make_meta(venue_id);
  metric.set_name(name);
  metric.set_value(value);
  *metric.mutable_timestamp() = cex::common::now_ts();

  (*metric.mutable_labels())["venue_id"] = venue_id;
  for (const auto& [k, v] : labels) {
    (*metric.mutable_labels())[k] = v;
  }

  return Produce(topics_.metrics, venue_id, cex::common::to_bytes(metric));
}

bool VenueObservabilityProducer::Produce(
    const std::string& topic,
    const std::string& key,
    const std::string& payload) {
  if (produce_fn_) return produce_fn_(topic, key, payload);
  if (!producer_) return false;
  return producer_->produce(topic, key, payload);
}

}  // namespace cex::venues::infra
