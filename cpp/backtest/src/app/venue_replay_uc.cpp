#include "app/venue_replay_uc.hpp"

#include "cex/common/log.hpp"

namespace cex::backtest::app {

VenueReplayUseCases::VenueReplayUseCases(IVenueReplayStorage* storage)
    : storage_(storage) {}

void VenueReplayUseCases::OnVenueSnapshot(
    const fob::venue::v1::VenueSnapshot& snapshot) {
  {
    std::lock_guard<std::mutex> lg(mu_);
    ++snapshots_received_;
    last_venue_id_ = snapshot.venue_id();
    last_symbol_ = snapshot.instrument().symbol();
  }

  if (storage_ == nullptr) {
    cex::common::log_json("WARN", "Venue replay storage is not configured",
                          {{"venue_id", snapshot.venue_id()},
                           {"symbol", snapshot.instrument().symbol()}});
    return;
  }

  const bool ok = storage_->SaveVenueSnapshot(snapshot);

  {
    std::lock_guard<std::mutex> lg(mu_);
    if (ok) ++snapshots_saved_;
  }

  cex::common::log_json("INFO", "Venue snapshot persisted for replay",
                        {{"venue_id", snapshot.venue_id()},
                         {"symbol", snapshot.instrument().symbol()},
                         {"status", snapshot.status()},
                         {"saved", ok ? "true" : "false"}});
  if (!ok) {
    cex::common::log_json("ERROR", "Failed to persist venue snapshot",
                          {{"venue_id", snapshot.venue_id()},
                           {"symbol", snapshot.instrument().symbol()}});
  }
}

void VenueReplayUseCases::OnVenueLiquidityCurve(
    const fob::venue::v1::VenueLiquidityCurve& curve) {
  {
    std::lock_guard<std::mutex> lg(mu_);
    ++curves_received_;
    last_venue_id_ = curve.venue_id();
    last_symbol_ = curve.instrument().symbol();
  }

  if (storage_ == nullptr) {
    cex::common::log_json("WARN", "Venue replay storage is not configured",
                          {{"venue_id", curve.venue_id()},
                           {"symbol", curve.instrument().symbol()}});
    return;
  }

  const bool ok = storage_->SaveVenueLiquidityCurve(curve);

  {
    std::lock_guard<std::mutex> lg(mu_);
    if (ok) ++curves_saved_;
  }

  cex::common::log_json("INFO", "Venue liquidity curve persisted for replay",
                        {{"venue_id", curve.venue_id()},
                         {"symbol", curve.instrument().symbol()},
                         {"level", curve.level()},
                         {"snapshot_id", curve.snapshot_id()},
                         {"saved", ok ? "true" : "false"}});
  if (!ok) {
    cex::common::log_json("ERROR", "Failed to persist venue liquidity curve",
                          {{"venue_id", curve.venue_id()},
                           {"symbol", curve.instrument().symbol()}});
  }
}

VenueReplayUseCases::Stats VenueReplayUseCases::GetStats() const {
  std::lock_guard<std::mutex> lg(mu_);
  return Stats{
      .snapshots_received = snapshots_received_,
      .snapshots_saved = snapshots_saved_,
      .curves_received = curves_received_,
      .curves_saved = curves_saved_,
      .last_venue_id = last_venue_id_,
      .last_symbol = last_symbol_,
  };
}

}  // namespace cex::backtest::app
