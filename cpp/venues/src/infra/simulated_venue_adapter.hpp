#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "domain/venue_adapter.hpp"

namespace cex::venues::infra {

// MVP adapter that emulates a remote venue API but keeps the same contract
// as a real CEX/DEX/AMM adapter.
class SimulatedVenueAdapter final : public domain::VenueAdapter {
 public:
  explicit SimulatedVenueAdapter(
      std::string venue_id,
      domain::VenueType venue_type = domain::VenueType::kCex);

  std::string VenueId() const override;
  domain::VenueType Type() const override;

  bool Connect() override;
  bool Subscribe(const std::vector<domain::VenueSubscription>& subscriptions) override;
  bool Reconnect() override;
  domain::VenueHeartbeat Heartbeat() override;

  std::optional<domain::VenueRawSnapshot> RequestSnapshot(
      const domain::VenueSnapshotRequest& request) override;

  domain::VenueOrderResult SendOrder(
      const fob::execution::v1::ExecutionIntent& intent) override;

  bool ApplyRuntimeConfig(const domain::VenueAdapterRuntimeConfig& config) override;

 private:
  fob::common::v1::Instrument ResolveInstrument(
      const domain::VenueSnapshotRequest& request) const;
  std::string ResolveVenueSymbol(
      const domain::VenueSnapshotRequest& request) const;

  mutable std::mutex mu_;
  std::string venue_id_;
  domain::VenueType venue_type_;
  bool connected_{false};
  uint32_t reconnect_attempts_{0};
  uint32_t consecutive_errors_{0};
  uint64_t sequence_{0};
  int64_t mid_price_units_{10000};  // tracks the latest simulated market mid
  int64_t last_snapshot_time_ms_{0};
  std::vector<domain::VenueSubscription> subscriptions_;
};

}  // namespace cex::venues::infra
