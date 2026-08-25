#pragma once

#include "fob/venue/v1/venue.grpc.pb.h"

#include "domain/entities/venue_state.hpp"

namespace cex::venue_health::infra::mappers {

fob::venue::v1::VenueHealth ToProto(const std::string& venue, const domain::VenueState&);

}  // namespace cex::venue_health::infra::mappers
