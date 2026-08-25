#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "domain/venue_adapter.hpp"

namespace cex::venues::domain {

// Configuration for status derivation.
struct SnapshotStatusConfig {
  // If no update received within this threshold, mark as stale.
  int64_t stale_threshold_ms{3000};
};

// F11-NORM-3: Derive snapshot status from raw data and age.
//
// Rules (applied in priority order):
//  1. If adapter reports kDisconnected → "disconnected"
//  2. If both bids and asks are empty → "empty"
//  3. If snapshot age exceeds stale_threshold → "stale"
//  4. Otherwise → "connected"
VenueConnectionStatus DeriveSnapshotStatus(
    const VenueRawSnapshot& raw,
    int64_t snapshot_age_ms,
    const SnapshotStatusConfig& config);

// Tracks last-seen timestamp per venue|symbol to compute snapshot age.
class SnapshotAgeTracker {
 public:
  // Record a snapshot arrival. Returns the age in ms since the previous
  // snapshot for the same venue|symbol (0 on first call).
  int64_t RecordAndGetAge(const std::string& venue_id,
                          const std::string& symbol);

  // Get the age of the last snapshot for venue|symbol (ms since last record).
  // Returns -1 if no snapshot was recorded.
  int64_t GetAge(const std::string& venue_id,
                 const std::string& symbol) const;

  // Get the timestamp (steady_clock ms) of the last snapshot.
  int64_t GetLastTimestampMs(const std::string& venue_id,
                             const std::string& symbol) const;

 private:
  using Clock = std::chrono::steady_clock;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Clock::time_point> last_seen_;

  static std::string Key(const std::string& venue_id, const std::string& symbol) {
    return venue_id + "|" + symbol;
  }
};

}  // namespace cex::venues::domain
