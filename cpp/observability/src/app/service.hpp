#pragma once

#include <unordered_map>

#include "app/alert.hpp"
#include "domain/entities/venue_health.hpp"
#include "domain/entities/venue_liquidity_curve.hpp"
#include "domain/ports/i_alert_publisher.hpp"

namespace cex::observability::app {

class Service {
 public:
  explicit Service(domain::IAlertPublisher&);

  void OnVenueHealth(const domain::VenueHealth&);
  void OnVenueLiquidityCurve(const domain::VenueLiquidityCurve&);

 private:
  void CheckAlert(const AlertCheck&);

  domain::IAlertPublisher& publisher_;
  std::unordered_map<AlertKey, domain::Alert> alerts_;
};

}  // namespace cex::observability::app
