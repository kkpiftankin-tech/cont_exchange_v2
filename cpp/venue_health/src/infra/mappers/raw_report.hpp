#pragma once

#include "fob/venue/v1/venue.pb.h"

#include "domain/entities/raw_report.hpp"

namespace cex::venue_health::infra::mappers {

domain::RawReport FromProto(const fob::venue::v1::VenueHealth&);

}
