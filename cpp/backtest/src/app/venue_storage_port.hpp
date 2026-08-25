#pragma once

#include "fob/venue/v1/venue.pb.h"

namespace cex::backtest::app {

// Port for persisting venue snapshots and liquidity curves for replay (F-15).
class IVenueReplayStorage {
 public:
  virtual ~IVenueReplayStorage() = default;
  virtual bool SaveVenueSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) = 0;
  virtual bool SaveVenueLiquidityCurve(const fob::venue::v1::VenueLiquidityCurve& curve) = 0;
};

}  // namespace cex::backtest::app
