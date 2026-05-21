#pragma once

#include <unordered_map>

#include "domain/entities/circuit_breaker.hpp"
#include "domain/entities/raw_report.hpp"
#include "domain/ports/i_health_state_publisher.hpp"

namespace cex::venue_health::app {

class Service {
 public:
  explicit Service(domain::IHealthStatePublisher&, const domain::Config&);

  void OnRawReport(const domain::RawReport&);

 private:
  domain::IHealthStatePublisher& publisher_;
  domain::Config config_;
  std::unordered_map<std::string, domain::VenueState> state_;
};

}  // namespace cex::venue_health::app