#pragma once
#include "domain/entities/venue_state.hpp"

namespace cex::venue_health::domain {

struct IHealthStatePublisher {
  virtual bool Publish(const VenueState&) = 0;

 protected:
  virtual ~IHealthStatePublisher() = default;
};

}  // namespace cex::venue_health::domain