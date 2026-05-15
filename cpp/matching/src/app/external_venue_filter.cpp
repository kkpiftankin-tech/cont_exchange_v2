#include "app/external_venue_filter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

#include "cex/common/proto.hpp"
#include "fob/observability/v1/observability.pb.h"

namespace cex::matching::app {

namespace {

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

void SetReason(std::string* reason, const std::string& value) {
  if (reason != nullptr) *reason = value;
}

bool SideHasLiquidity(const fob::venue::v1::SideLiquidityCurve& side) {
  const int q_size = side.q_grid_size();
  const int p_size = side.p_of_q_size();
  if (q_size == 0 || p_size == 0 || q_size != p_size) {
    return false;
  }

  bool has_positive_qty = false;
  double prev_qty = 0.0;

  for (int i = 0; i < q_size; ++i) {
    const double qty = side.q_grid(i);
    const double price = side.p_of_q(i);

    if (!std::isfinite(qty) || !std::isfinite(price)) {
      return false;
    }
    if (qty < 0.0 || price <= 0.0) {
      return false;
    }
    if (i > 0 && qty < prev_qty) {
      return false;
    }

    has_positive_qty = has_positive_qty || qty > 0.0;
    prev_qty = qty;
  }

  return has_positive_qty;
}

bool ContainsKeyword(const std::string& value, const std::string& keyword) {
  return ToLower(value).find(keyword) != std::string::npos;
}

std::optional<fob::venue::v1::VenueHealthStatus> LegacyStatusToHealthStatus(
    const std::string& status) {
  const std::string lowered = ToLower(status);
  if (lowered == "connected") return fob::venue::v1::VENUE_HEALTH_STATUS_OK;
  if (lowered == "stale") return fob::venue::v1::VENUE_HEALTH_STATUS_STALE;
  if (lowered == "disconnected") return fob::venue::v1::VENUE_HEALTH_STATUS_DISCONNECTED;
  if (lowered == "empty" || lowered == "disabled" || lowered == "degraded") {
    return fob::venue::v1::VENUE_HEALTH_STATUS_DEGRADED;
  }
  if (lowered == "rate_limit") return fob::venue::v1::VENUE_HEALTH_STATUS_RATE_LIMIT;
  return std::nullopt;
}

fob::venue::v1::RoutingRecommendation LegacyStatusToRouting(
    const std::string& status) {
  const std::string lowered = ToLower(status);
  if (lowered == "connected") return fob::venue::v1::ROUTING_RECOMMENDATION_ALLOW;
  if (lowered == "stale") return fob::venue::v1::ROUTING_RECOMMENDATION_AVOID;
  return fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK;
}

double LegacyStatusToHealthScore(const std::string& status) {
  const std::string lowered = ToLower(status);
  if (lowered == "connected") return 1.0;
  if (lowered == "stale") return 0.25;
  return 0.0;
}

std::optional<fob::venue::v1::VenueHealth> ParseLegacyVenueHealth(
    const std::string& payload) {
  fob::observability::v1::LogEvent event;
  if (!cex::common::from_bytes(payload, event) || event.message() != "VENUE_STATUS") {
    return std::nullopt;
  }

  const auto venue_it = event.fields().find("venue_id");
  const auto status_it = event.fields().find("status");
  if (venue_it == event.fields().end() || status_it == event.fields().end()) {
    return std::nullopt;
  }

  const auto maybe_status = LegacyStatusToHealthStatus(status_it->second);
  if (!maybe_status.has_value()) {
    return std::nullopt;
  }

  fob::venue::v1::VenueHealth health;
  if (event.has_meta()) *health.mutable_meta() = event.meta();
  health.set_venue(venue_it->second);
  if (event.has_timestamp()) {
    *health.mutable_timestamp() = event.timestamp();
  } else if (event.has_meta() && event.meta().has_ts_event()) {
    *health.mutable_timestamp() = event.meta().ts_event();
  }
  health.set_event_type(fob::venue::v1::VENUE_HEALTH_EVENT_TYPE_RAW);
  health.set_status(*maybe_status);
  health.set_routing_recommendation(LegacyStatusToRouting(status_it->second));
  health.set_health_score(LegacyStatusToHealthScore(status_it->second));

  const auto reason_it = event.fields().find("reason");
  if (reason_it != event.fields().end()) {
    health.set_reason(reason_it->second);
  } else {
    health.set_reason(status_it->second);
  }

  return health;
}

}  // namespace

std::optional<fob::venue::v1::VenueHealth> ParseVenueHealthMessage(
    const std::string& payload) {
  fob::venue::v1::VenueHealth health;
  if (cex::common::from_bytes(payload, health)) {
    return health;
  }
  return ParseLegacyVenueHealth(payload);
}

bool CurveHasUsableLiquidity(const fob::venue::v1::VenueLiquidityCurve& curve) {
  return SideHasLiquidity(curve.bid_curve()) || SideHasLiquidity(curve.ask_curve());
}

bool VenueAllowsMatching(const fob::venue::v1::VenueHealth& health,
                         VenueThresholds thresholds,
                         std::string* reason) {
  if (ContainsKeyword(health.reason(), "disable")) {
    SetReason(reason, "disabled_reason");
    return false;
  }

  if (health.breaker_state() == fob::venue::v1::CIRCUIT_BREAKER_STATE_OPEN) {
    SetReason(reason, "circuit_breaker_open");
    return false;
  }

  switch (health.routing_recommendation()) {
    case fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK:
      SetReason(reason, "routing_block");
      return false;
    case fob::venue::v1::ROUTING_RECOMMENDATION_AVOID:
      SetReason(reason, "routing_avoid");
      return false;
    default:
      break;
  }

  switch (health.status()) {
    case fob::venue::v1::VENUE_HEALTH_STATUS_STALE:
      SetReason(reason, "status_stale");
      return false;
    case fob::venue::v1::VENUE_HEALTH_STATUS_DISCONNECTED:
      SetReason(reason, "status_disconnected");
      return false;
    case fob::venue::v1::VENUE_HEALTH_STATUS_RATE_LIMIT:
      SetReason(reason, "status_rate_limit");
      return false;
    case fob::venue::v1::VENUE_HEALTH_STATUS_DEGRADED:
      SetReason(reason, "status_degraded");
      return false;
    default:
      break;
  }

  if (health.health_score() < thresholds.min_health_score) {
    SetReason(reason, "health_score");
    return false;
  }
  return true;
}

bool ShouldUseVenueCurveForMatching(const fob::venue::v1::VenueLiquidityCurve& curve,
                                    const fob::venue::v1::VenueHealth* health,
                                    VenueThresholds thresholds,
                                    std::string* reason) {
  if (curve.venue_id().empty() || curve.instrument().symbol().empty()) {
    SetReason(reason, "missing_curve_identity");
    return false;
  }

  if (!CurveHasUsableLiquidity(curve)) {
    SetReason(reason, "empty_curve");
    return false;
  }

  if (!std::isfinite(curve.confidence()) || curve.confidence() < 0.0) {
    SetReason(reason, "invalid_confidence");
    return false;
  }

  if (health != nullptr) {
    if (!VenueAllowsMatching(*health, thresholds, reason)) {
      return false;
    }
  }

  return true;
}

}  // namespace cex::matching::app
