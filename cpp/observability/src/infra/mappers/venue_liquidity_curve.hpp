#pragma once

#include "fob/venue/v1/venue.pb.h"

#include "domain/entities/venue_liquidity_curve.hpp"

namespace cex::observability::infra::mappers {

domain::VenueLiquidityCurve FromProto(const fob::venue::v1::VenueLiquidityCurve&);

}  // namespace cex::observability::infra::mappers
