#pragma once

#include "fob/venue/v1/venue.pb.h"

#include "domain/entities/venue_health.hpp"

namespace cex::observability::infra::mappers {

domain::VenueHealth FromProto(const fob::venue::v1::VenueHealth&);

}  // namespace cex::observability::infra::mappers
