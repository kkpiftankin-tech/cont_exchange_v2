#include "app/venue_sim_router.hpp"

namespace cex::venues::app {

RouteDecision VenueSimRouter::Decide(const std::string& venue_id,
                                     const std::string& symbol) const {
  RouteDecision d;  // defaults: LIVE_ONLY, no session
  auto session = registry_.Resolve(venue_id, symbol);
  if (!session.has_value()) {
    return d;
  }

  d.has_session = true;
  d.sim_session_id = session->sim_session_id();
  d.mode = session->routing_mode();
  // A governing session with an unset mode is a misconfiguration; fall back
  // to LIVE_ONLY so we never route to sim by accident.
  if (d.mode == fob::sim::v1::ROUTING_MODE_UNSPECIFIED) {
    d.mode = fob::sim::v1::ROUTING_MODE_LIVE_ONLY;
  }

  d.models.latency = session->latency_model();
  d.models.impact = session->impact_model();
  d.models.fee = session->fee_model();
  d.models.rejection = session->rejection_model();

  if (session->stale_lob_threshold_ms() > 0) {
    d.stale_lob_threshold_ms = session->stale_lob_threshold_ms();
  }
  if (session->partial_fill_mode() !=
      fob::sim::v1::PARTIAL_FILL_MODE_UNSPECIFIED) {
    d.partial_fill_mode = session->partial_fill_mode();
  }
  return d;
}

}  // namespace cex::venues::app
