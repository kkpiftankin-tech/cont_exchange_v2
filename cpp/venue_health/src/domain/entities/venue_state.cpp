#include "venue_state.hpp"

namespace cex::venue_health::domain {

VenueState::VenueState(const Config& config, std::string venue)
    : config_(config),
      venue_(std::move(venue)),
      score(1.0),
      routing(RoutingRecommendation::Allow),
      updated_at_(Timestamp::clock::now()),
      status_(VenueHealthStatus::Unspecified),
      latency_(Duration::zero()),
      stale_(Duration::zero()),
      error_rate_(0.0),
      error_count_(0),
      snapshots_per_sec_(0.0),
      last_snapshot_age_(0.0),
      reason_(),
      breaker_(config) {}

void VenueState::OnRawReport(const RawReport& report) {
  breaker_.OnRawReport(report);
  updated_at_ = report.timestamp;
  status_ = report.venue_status;
  latency_ = report.latency;
  stale_ = report.stale;
  error_rate_ = report.error_rate;
  error_count_ = report.error_count;
  snapshots_per_sec_ = report.snapshots_per_sec;
  last_snapshot_age_ = report.last_snapshot_age;
  reason_ = report.reason;

  score = ((1 - report.error_rate) + score) / 2;
  routing = RoutingRecommendation::Allow;

  if (report.venue_status == VenueHealthStatus::Stale || report.stale > config_.stale_threshold) {
    score = std::min(score, 0.2);
    routing = std::min(routing, RoutingRecommendation::Avoid);
  }

  if (report.latency > std::chrono::milliseconds(500) || report.snapshots_per_sec < 2) {
    score = std::min(score, 0.5);
    routing = std::min(routing, RoutingRecommendation::Caution);
  }

  if (report.venue_status == VenueHealthStatus::RateLimit) {
    score = std::min(score, 0.8);
    routing = std::min(routing, RoutingRecommendation::Caution);
  }

  switch (breaker_.State()) {
    case CircuitBreakerState::Open:
      score = 0;
      routing = RoutingRecommendation::Block;
      return;
    case CircuitBreakerState::HalfOpen:
      score = 0.1;
      routing = RoutingRecommendation::Avoid;
      break;
    case CircuitBreakerState::Closed:
      break;
  }
}

std::string VenueState::Venue() const { return venue_; }

double VenueState::Score() const { return score; }

RoutingRecommendation VenueState::Routing() const { return routing; }

CircuitBreakerState VenueState::BreakerState() const { return breaker_.State(); }

Timestamp VenueState::UpdatedAt() const { return updated_at_; }

VenueHealthStatus VenueState::Status() const { return status_; }

Duration VenueState::Latency() const { return latency_; }

Duration VenueState::Stale() const { return stale_; }

double VenueState::ErrorRate() const { return error_rate_; }

uint32_t VenueState::ErrorCount() const { return error_count_; }

double VenueState::SnapshotsPerSec() const { return snapshots_per_sec_; }

double VenueState::LastSnapshotAge() const { return last_snapshot_age_; }

const std::string& VenueState::Reason() const { return reason_; }

}  // namespace cex::venue_health::domain
