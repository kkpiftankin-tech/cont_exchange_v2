#include <cassert>
#include <limits>
#include <string>

#include "app/external_venue_filter.hpp"
#include "cex/common/proto.hpp"
#include "fob/observability/v1/observability.pb.h"

namespace {

cex::matching::app::VenueThresholds DefaultThresholds() {
  cex::matching::app::VenueThresholds thresholds;
  thresholds.min_health_score = 0.5;
  thresholds.min_confidence    = 0.2;
  thresholds.confidence_for_l3     = 0.8;
  thresholds.confidence_for_l2     = 0.5;
  return thresholds;
}

fob::venue::v1::VenueLiquidityCurve MakeCurve(const bool with_liquidity = true) {
  fob::venue::v1::VenueLiquidityCurve curve;
  curve.set_venue_id("binance");
  curve.mutable_instrument()->set_symbol("BTC/USDT");
  curve.set_confidence(1.0);
  curve.set_level("L3");
  if (!with_liquidity) return curve;

  auto* ask = curve.mutable_ask_curve();
  ask->add_q_grid(1.0);
  ask->add_p_of_q(101.0);
  return curve;
}

fob::venue::v1::VenueHealth MakeHealth(
    const fob::venue::v1::VenueHealthStatus status,
    const fob::venue::v1::RoutingRecommendation routing,
    const double score = 1.0,
    const fob::venue::v1::CircuitBreakerState breaker =
        fob::venue::v1::CIRCUIT_BREAKER_STATE_CLOSED) {
  fob::venue::v1::VenueHealth health;
  health.set_venue("binance");
  health.set_status(status);
  health.set_routing_recommendation(routing);
  health.set_health_score(score);
  health.set_breaker_state(breaker);
  return health;
}

std::string MakeLegacyStatusEvent(const std::string& venue_id,
                                  const std::string& status) {
  fob::observability::v1::LogEvent event;
  event.set_message("VENUE_STATUS");
  (*event.mutable_fields())["venue_id"] = venue_id;
  (*event.mutable_fields())["status"] = status;
  return cex::common::to_bytes(event);
}

void test_accepts_curve_without_health() {
  const auto curve = MakeCurve();
  assert(cex::matching::app::ShouldUseVenueCurveForMatching(curve, nullptr, DefaultThresholds()));
}

void test_rejects_empty_curve() {
  const auto curve = MakeCurve(false);
  std::string reason;
  assert(!cex::matching::app::ShouldUseVenueCurveForMatching(curve, nullptr, DefaultThresholds(), &reason));
  assert(reason == "empty_curve");
}

void test_rejects_blocked_health() {
  const auto curve = MakeCurve();
  const auto health = MakeHealth(fob::venue::v1::VENUE_HEALTH_STATUS_OK,
                                 fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK);
  std::string reason;
  assert(!cex::matching::app::ShouldUseVenueCurveForMatching(curve, &health, DefaultThresholds(), &reason));
  assert(reason == "routing_block");
}

void test_rejects_low_health_score() {
  const auto curve = MakeCurve();
  const auto health = MakeHealth(fob::venue::v1::VENUE_HEALTH_STATUS_OK,
                                 fob::venue::v1::ROUTING_RECOMMENDATION_ALLOW,
                                 0.4);
  std::string reason;
  assert(!cex::matching::app::ShouldUseVenueCurveForMatching(curve, &health, DefaultThresholds(), &reason));
  assert(reason == "health_score");
}

void test_rejects_curve_with_mismatched_depth_arrays() {
  auto curve = MakeCurve();
  curve.mutable_ask_curve()->add_q_grid(2.0);

  std::string reason;
  assert(!cex::matching::app::ShouldUseVenueCurveForMatching(curve, nullptr, DefaultThresholds(), &reason));
  assert(reason == "empty_curve");
}

void test_rejects_curve_with_non_finite_values() {
  auto curve = MakeCurve();
  curve.mutable_ask_curve()->set_p_of_q(0, std::numeric_limits<double>::quiet_NaN());

  std::string reason;
  assert(!cex::matching::app::ShouldUseVenueCurveForMatching(curve, nullptr, DefaultThresholds(), &reason));
  assert(reason == "empty_curve");
}

void test_accepts_low_confidence_curve_when_shape_is_valid() {
  auto curve = MakeCurve();
  curve.set_confidence(0.01);

  std::string reason;
  assert(cex::matching::app::ShouldUseVenueCurveForMatching(
      curve, nullptr, DefaultThresholds(), &reason));
  assert(reason.empty());
}

void test_rejects_curve_with_invalid_confidence() {
  auto curve = MakeCurve();
  curve.set_confidence(std::numeric_limits<double>::quiet_NaN());

  std::string reason;
  assert(!cex::matching::app::ShouldUseVenueCurveForMatching(
      curve, nullptr, DefaultThresholds(), &reason));
  assert(reason == "invalid_confidence");
}

void test_rejects_curve_without_identity() {
  auto curve = MakeCurve();
  curve.clear_venue_id();

  std::string reason;
  assert(!cex::matching::app::ShouldUseVenueCurveForMatching(curve, nullptr, DefaultThresholds(), &reason));
  assert(reason == "missing_curve_identity");
}

void test_parses_legacy_status_event() {
  const auto payload = MakeLegacyStatusEvent("binance", "disconnected");
  const auto parsed = cex::matching::app::ParseVenueHealthMessage(payload);
  assert(parsed.has_value());
  assert(parsed->venue() == "binance");
  assert(parsed->status() == fob::venue::v1::VENUE_HEALTH_STATUS_DISCONNECTED);
  assert(parsed->routing_recommendation() == fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK);
}

}  // namespace

int main() {
  test_accepts_curve_without_health();
  test_rejects_empty_curve();
  test_rejects_blocked_health();
  test_rejects_low_health_score();
  test_rejects_curve_with_mismatched_depth_arrays();
  test_rejects_curve_with_non_finite_values();
  test_accepts_low_confidence_curve_when_shape_is_valid();
  test_rejects_curve_with_invalid_confidence();
  test_rejects_curve_without_identity();
  test_parses_legacy_status_event();
  return 0;
}
