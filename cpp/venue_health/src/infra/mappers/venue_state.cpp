#include "venue_state.hpp"

#include <google/protobuf/util/time_util.h>

namespace cex::venue_health::infra::mappers {

namespace detail {

fob::venue::v1::CircuitBreakerState ToProto(domain::CircuitBreakerState state) {
  switch (state) {
    case domain::CircuitBreakerState::Closed:
      return fob::venue::v1::CircuitBreakerState::CIRCUIT_BREAKER_STATE_CLOSED;
    case domain::CircuitBreakerState::HalfOpen:
      return fob::venue::v1::CircuitBreakerState::CIRCUIT_BREAKER_STATE_HALF_OPEN;
    case domain::CircuitBreakerState::Open:
      return fob::venue::v1::CircuitBreakerState::CIRCUIT_BREAKER_STATE_OPEN;
    default:
      throw std::invalid_argument("unknown CircuitBreakerState");
  }
}

google::protobuf::Timestamp ToProto(domain::Timestamp ts) {
  auto nanos =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(ts).time_since_epoch().count();
  return google::protobuf::util::TimeUtil::NanosecondsToTimestamp(nanos);
}

fob::venue::v1::RoutingRecommendation FromProto(domain::RoutingRecommendation routing) {
  switch (routing) {
    case domain::RoutingRecommendation::Block:
      return fob::venue::v1::RoutingRecommendation::ROUTING_RECOMMENDATION_BLOCK;
    case domain::RoutingRecommendation::Avoid:
      return fob::venue::v1::RoutingRecommendation::ROUTING_RECOMMENDATION_AVOID;
    case domain::RoutingRecommendation::Caution:
      return fob::venue::v1::RoutingRecommendation::ROUTING_RECOMMENDATION_CAUTION;
    case domain::RoutingRecommendation::Allow:
      return fob::venue::v1::RoutingRecommendation::ROUTING_RECOMMENDATION_ALLOW;
    default:
      throw std::invalid_argument("unknown RoutingRecommendation");
  }
}

fob::venue::v1::VenueHealthStatus FromProto(domain::VenueHealthStatus status) {
  switch (status) {
    case domain::VenueHealthStatus::Unspecified:
      return fob::venue::v1::VENUE_HEALTH_STATUS_UNSPECIFIED;
    case domain::VenueHealthStatus::Ok:
      return fob::venue::v1::VENUE_HEALTH_STATUS_OK;
    case domain::VenueHealthStatus::Stale:
      return fob::venue::v1::VENUE_HEALTH_STATUS_STALE;
    case domain::VenueHealthStatus::Disconnected:
      return fob::venue::v1::VENUE_HEALTH_STATUS_DISCONNECTED;
    case domain::VenueHealthStatus::RateLimit:
      return fob::venue::v1::VENUE_HEALTH_STATUS_RATE_LIMIT;
    case domain::VenueHealthStatus::Degraded:
      return fob::venue::v1::VENUE_HEALTH_STATUS_DEGRADED;
    default:
      throw std::invalid_argument("unknown VenueHealthStatus");
  }
}

}  // namespace detail

fob::venue::v1::VenueHealth ToProto(const std::string& venue, const domain::VenueState& state) {
  fob::venue::v1::VenueHealth result;
  result.set_venue(venue);
  *result.mutable_timestamp() = detail::ToProto(state.UpdatedAt());
  result.set_event_type(fob::venue::v1::VENUE_HEALTH_EVENT_TYPE_AGGREGATED);
  result.set_breaker_state(detail::ToProto(state.BreakerState()));
  result.set_status(detail::FromProto(state.Status()));
  result.set_latency_ms(static_cast<uint32_t>(std::max<int64_t>(
      0, state.Latency().count())));
  result.set_stale_ms(static_cast<uint32_t>(std::max<int64_t>(
      0, state.Stale().count())));
  result.set_error_rate(state.ErrorRate());
  result.set_error_count(state.ErrorCount());
  result.set_snapshots_per_sec(state.SnapshotsPerSec());
  result.set_last_snapshot_age(state.LastSnapshotAge());
  result.set_health_score(state.Score());
  result.set_routing_recommendation(detail::FromProto(state.Routing()));
  result.set_reason(state.Reason());
  return result;
}

}  // namespace cex::venue_health::infra::mappers
