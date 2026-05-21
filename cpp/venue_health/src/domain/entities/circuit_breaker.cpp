#include "circuit_breaker.hpp"

namespace cex::venue_health::domain {

namespace detail {

bool IsError(VenueHealthStatus status) {
  switch (status) {
    case VenueHealthStatus::Ok:
    case VenueHealthStatus::RateLimit:
      return false;
    default:
      return true;
  }
}

}  // namespace detail

CircuitBreaker::CircuitBreaker(const Config& config)
    : config_(config), state_(CircuitBreakerState::Closed) {}

void CircuitBreaker::OnRawReport(const RawReport& report) {
  auto border = Timestamp::clock::now() - config_.circuit_breaker_window;
  error_timestamps_.erase(error_timestamps_.begin(), error_timestamps_.lower_bound(border));

  if (detail::IsError(report.venue_status)) {
    error_timestamps_.insert(report.timestamp);
  }

  if (error_timestamps_.size() >= config_.circuit_breaker_error_threshold) {
    cooldown_end_ = Timestamp::clock::now() + config_.circuit_breaker_cooldown;
    state_ = CircuitBreakerState::Open;
    return;
  }

  switch (state_) {
    case CircuitBreakerState::Open:
      if (Timestamp::clock::now() >= cooldown_end_) {
        state_ = CircuitBreakerState::HalfOpen;
      }
      break;
    case CircuitBreakerState::HalfOpen:
      if (report.venue_status == VenueHealthStatus::Ok) {
        state_ = CircuitBreakerState::Closed;
      } else {
        cooldown_end_ = Timestamp::clock::now() + config_.circuit_breaker_cooldown;
        state_ = CircuitBreakerState::Open;
      }
      break;
    case CircuitBreakerState::Closed:
      break;
  }
}

CircuitBreakerState CircuitBreaker::State() const { return state_; }

}  // namespace cex::venue_health::domain
