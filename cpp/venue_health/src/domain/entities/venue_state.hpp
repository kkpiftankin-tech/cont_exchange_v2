#pragma once

#include "circuit_breaker.hpp"

namespace cex::venue_health::domain {

enum class RoutingRecommendation {
  Block,
  Avoid,
  Caution,
  Allow,
};

class VenueState {
 public:
  VenueState(const Config&, std::string venue);

  void OnRawReport(const RawReport&);

  [[nodiscard]] std::string Venue() const;
  [[nodiscard]] double Score() const;
  [[nodiscard]] RoutingRecommendation Routing() const;
  [[nodiscard]] CircuitBreakerState BreakerState() const;
  [[nodiscard]] Timestamp UpdatedAt() const;
  [[nodiscard]] VenueHealthStatus Status() const;
  [[nodiscard]] Duration Latency() const;
  [[nodiscard]] Duration Stale() const;
  [[nodiscard]] double ErrorRate() const;
  [[nodiscard]] uint32_t ErrorCount() const;
  [[nodiscard]] double SnapshotsPerSec() const;
  [[nodiscard]] double LastSnapshotAge() const;
  [[nodiscard]] const std::string& Reason() const;

 private:
  Config config_;

  std::string venue_;
  double score;
  RoutingRecommendation routing;
  Timestamp updated_at_;
  VenueHealthStatus status_;
  Duration latency_;
  Duration stale_;
  double error_rate_;
  uint32_t error_count_;
  double snapshots_per_sec_;
  double last_snapshot_age_;
  std::string reason_;
  CircuitBreaker breaker_;
};

}  // namespace cex::venue_health::domain
