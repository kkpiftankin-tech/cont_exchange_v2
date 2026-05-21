#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "app/venue_storage_port.hpp"

namespace cex::backtest::app {

// Receives VenueSnapshot and VenueLiquidityCurve events from Kafka,
// persists them for replay (F11-BACKTEST-1, feeds F-15).
class VenueReplayUseCases {
 public:
  struct Stats {
    uint64_t snapshots_received{0};
    uint64_t snapshots_saved{0};
    uint64_t curves_received{0};
    uint64_t curves_saved{0};
    std::string last_venue_id;
    std::string last_symbol;
  };

  explicit VenueReplayUseCases(IVenueReplayStorage* storage = nullptr);

  void OnVenueSnapshot(const fob::venue::v1::VenueSnapshot& snapshot);
  void OnVenueLiquidityCurve(const fob::venue::v1::VenueLiquidityCurve& curve);
  Stats GetStats() const;

 private:
  IVenueReplayStorage* storage_{nullptr};
  mutable std::mutex mu_;
  uint64_t snapshots_received_{0};
  uint64_t snapshots_saved_{0};
  uint64_t curves_received_{0};
  uint64_t curves_saved_{0};
  std::string last_venue_id_;
  std::string last_symbol_;
};

}  // namespace cex::backtest::app
