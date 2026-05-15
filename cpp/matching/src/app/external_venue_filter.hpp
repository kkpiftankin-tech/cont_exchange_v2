#pragma once

#include <optional>
#include <string>

#include "fob/venue/v1/venue.pb.h"
#include "app/planner_inputs_cache.hpp"

namespace cex::matching::app {

std::optional<fob::venue::v1::VenueHealth> ParseVenueHealthMessage(
    const std::string& payload);

bool CurveHasUsableLiquidity(const fob::venue::v1::VenueLiquidityCurve& curve);

bool VenueAllowsMatching(const fob::venue::v1::VenueHealth& health,
                         VenueThresholds thresholds,
                         std::string* reason = nullptr);

bool ShouldUseVenueCurveForMatching(const fob::venue::v1::VenueLiquidityCurve& curve,
                                    const fob::venue::v1::VenueHealth* health,
                                    VenueThresholds thresholds,
                                    std::string* reason = nullptr);

}  // namespace cex::matching::app
