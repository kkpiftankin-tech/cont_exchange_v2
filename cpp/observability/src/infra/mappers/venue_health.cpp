#include "venue_health.hpp"

namespace cex::observability::infra::mappers {

namespace detail {

domain::CircuitBreakerState FromProto(fob::venue::v1::CircuitBreakerState state) {
  switch (state) {
    case fob::venue::v1::CIRCUIT_BREAKER_STATE_CLOSED:
      return domain::CircuitBreakerState::Closed;
    case fob::venue::v1::CIRCUIT_BREAKER_STATE_OPEN:
      return domain::CircuitBreakerState::Open;
    case fob::venue::v1::CIRCUIT_BREAKER_STATE_HALF_OPEN:
      return domain::CircuitBreakerState::HalfOpen;
    default:
      throw std::invalid_argument("Unknown CircuitBreakerState");
  }
}

}  // namespace detail

domain::VenueHealth FromProto(const fob::venue::v1::VenueHealth& proto) {
  domain::VenueHealth result;
  result.venue = proto.venue();
  result.score = proto.health_score();
  result.breaker_state = detail::FromProto(proto.breaker_state());
  return result;
}

}  // namespace cex::observability::infra::mappers
