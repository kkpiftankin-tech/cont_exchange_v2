#pragma once

#include <string>

namespace cex::observability::domain {

enum class CircuitBreakerState {
  Closed,
  HalfOpen,
  Open,
};

struct VenueHealth {
  std::string venue;
  double score;
  CircuitBreakerState breaker_state;
};

}  // namespace cex::observability::domain
