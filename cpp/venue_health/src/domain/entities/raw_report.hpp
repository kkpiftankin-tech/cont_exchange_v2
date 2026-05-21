#pragma once

#include <chrono>
#include <string>

namespace cex::venue_health::domain {

using Timestamp = std::chrono::time_point<std::chrono::system_clock>;

using Duration = std::chrono::milliseconds;

enum class VenueHealthStatus {
  Unspecified,
  Ok,
  Stale,
  Disconnected,
  RateLimit,
  Degraded,
};

struct RawReport {
  std::string venue;
  Timestamp timestamp;
  VenueHealthStatus venue_status;
  Duration latency;
  Duration stale;
  double error_rate;
  uint32_t error_count;
  double snapshots_per_sec;
  double last_snapshot_age;
  std::string reason;
};

}  // namespace cex::venue_health::domain