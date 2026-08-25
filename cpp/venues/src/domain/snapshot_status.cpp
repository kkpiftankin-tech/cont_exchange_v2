#include "domain/snapshot_status.hpp"

namespace cex::venues::domain {

VenueConnectionStatus DeriveSnapshotStatus(
    const VenueRawSnapshot& raw,
    int64_t snapshot_age_ms,
    const SnapshotStatusConfig& config) {
  // 1. Adapter-level disconnect takes priority.
  if (raw.status == VenueConnectionStatus::kDisconnected) {
    return VenueConnectionStatus::kDisconnected;
  }

  // 2. Empty order book.
  if (raw.bids.empty() && raw.asks.empty()) {
    return VenueConnectionStatus::kEmpty;
  }

  // 3. Stale if age exceeds threshold.
  if (snapshot_age_ms > config.stale_threshold_ms) {
    return VenueConnectionStatus::kStale;
  }

  // 4. Otherwise connected.
  return VenueConnectionStatus::kConnected;
}

int64_t SnapshotAgeTracker::RecordAndGetAge(const std::string& venue_id,
                                            const std::string& symbol) {
  const auto now = Clock::now();
  const std::string key = Key(venue_id, symbol);

  std::lock_guard<std::mutex> lock(mu_);
  auto it = last_seen_.find(key);
  if (it == last_seen_.end()) {
    last_seen_[key] = now;
    return 0;
  }

  const int64_t age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - it->second).count();
  it->second = now;
  return age_ms;
}

int64_t SnapshotAgeTracker::GetAge(const std::string& venue_id,
                                   const std::string& symbol) const {
  const std::string key = Key(venue_id, symbol);

  std::lock_guard<std::mutex> lock(mu_);
  auto it = last_seen_.find(key);
  if (it == last_seen_.end()) return -1;

  const auto now = Clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      now - it->second).count();
}

int64_t SnapshotAgeTracker::GetLastTimestampMs(const std::string& venue_id,
                                                const std::string& symbol) const {
  const std::string key = Key(venue_id, symbol);

  std::lock_guard<std::mutex> lock(mu_);
  auto it = last_seen_.find(key);
  if (it == last_seen_.end()) return -1;

  return std::chrono::duration_cast<std::chrono::milliseconds>(
      it->second.time_since_epoch()).count();
}

}  // namespace cex::venues::domain
