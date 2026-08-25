#include "service.hpp"

namespace cex::venue_health::app {

Service::Service(domain::IHealthStatePublisher& publisher, const domain::Config& config)
    : publisher_(publisher), config_(config) {}

void Service::OnRawReport(const domain::RawReport& report) {
  if (!state_.contains(report.venue)) {
    state_.emplace(report.venue, domain::VenueState{config_, report.venue});
  };
  auto& state = state_.at(report.venue);
  state.OnRawReport(report);
  publisher_.Publish(state);
}

}  // namespace cex::venue_health::app
