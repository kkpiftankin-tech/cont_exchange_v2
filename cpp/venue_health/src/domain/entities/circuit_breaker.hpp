#pragma once

#include <set>

#include "config.hpp"

namespace cex::venue_health::domain {

enum class CircuitBreakerState {
  Closed,
  HalfOpen,
  Open,
};

class CircuitBreaker {
 public:
  explicit CircuitBreaker(const Config&);

  void OnRawReport(const RawReport&);

  [[nodiscard]] CircuitBreakerState State() const;

 private:
  Config config_;
  CircuitBreakerState state_;
  std::set<Timestamp> error_timestamps_;
  Timestamp cooldown_end_;
};

}  // namespace cex::venue_health::domain
