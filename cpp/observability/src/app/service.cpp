#include "app/service.hpp"

#include "cex/common/uuid.hpp"

namespace cex::observability::app {

Service::Service(domain::IAlertPublisher& publisher) : publisher_(publisher) {}

void Service::CheckAlert(const AlertCheck& check) {
  AlertKey key{.venue = check.venue, .code = check.code};
  auto it = alerts_.find(key);

  if (check.should_fire && it == alerts_.end()) {
    domain::Alert alert{.id = common::uuid_v4(),
                        .code = check.code,
                        .labels = {{"venue", check.venue}},
                        .severity = check.severity,
                        .status = domain::AlertStatus::Firing};
    alerts_.emplace(key, alert);
    publisher_.Publish(alert);
  } else if (check.should_resolve && it != alerts_.end()) {
    it->second.status = domain::AlertStatus::Resolved;
    publisher_.Publish(it->second);
    alerts_.erase(it);
  }
}

void Service::OnVenueHealth(const domain::VenueHealth& report) {
  constexpr double kVenueHealthScoreThreshold = 0.8;

  CheckAlert({.venue = report.venue,
              .code = domain::AlertCode::LowVenueHealthScore,
              .should_fire = report.score < kVenueHealthScoreThreshold,
              .should_resolve = report.score >= kVenueHealthScoreThreshold,
              .severity = domain::Severity::Warning});

  CheckAlert({.venue = report.venue,
              .code = domain::AlertCode::CircuitBreakerOpen,
              .should_fire = report.breaker_state == domain::CircuitBreakerState::Open,
              .should_resolve = report.breaker_state == domain::CircuitBreakerState::Closed,
              .severity = domain::Severity::Error});
}

void Service::OnVenueLiquidityCurve(const domain::VenueLiquidityCurve& curve) {
  constexpr double kLiquidityCurveConfidenceThreshold = 0.8;

  CheckAlert({.venue = curve.venue,
              .code = domain::AlertCode::LowLiquidityCurveConfidence,
              .should_fire = curve.confidence < kLiquidityCurveConfidenceThreshold,
              .should_resolve = curve.confidence >= kLiquidityCurveConfidenceThreshold,
              .severity = domain::Severity::Warning});
}

}  // namespace cex::observability::app
