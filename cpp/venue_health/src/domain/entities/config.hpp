#pragma once

#include "raw_report.hpp"

namespace cex::venue_health::domain {

struct Config {
  Duration circuit_breaker_window;
  Duration circuit_breaker_cooldown;
  uint32_t circuit_breaker_error_threshold;
  Duration stale_threshold;
};

}  // namespace cex::venue_health::domain
