#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "domain/normalize_snapshot.hpp"
#include "domain/snapshot_status.hpp"
#include "domain/venue_adapter.hpp"
#include "fob/venue/v1/venue.pb.h"

namespace cex::venues::app {

// Port: message publisher (Kafka or fake for testing).
class IMessagePublisher {
 public:
  virtual ~IMessagePublisher() = default;
  virtual bool Publish(const std::string& topic,
                       const std::string& key,
                       const std::string& payload) = 0;
};

// Port: snapshot storage (e.g. ClickHouse).
class ISnapshotStorage {
 public:
  virtual ~ISnapshotStorage() = default;
  virtual bool SaveSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) = 0;
};

// Configuration for the snapshot producer.
struct SnapshotProducerConfig {
  domain::NormalizationConfig normalization;
  domain::SnapshotStatusConfig status;
  std::string topic{"venue.snapshots"};
  bool emit_publish_log{false};
};

// F11-NORM-4: Idempotent producer for venue.snapshots.
//
// Integrates:
//  - F11-NORM-3: Status derivation with age tracking
//  - F11-NORM-1/2: NormalizeSnapshot
//  - F11-NORM-4: Kafka publish with sequence-based dedup
//  - F11-NORM-5: Optional ClickHouse persistence
class SnapshotProducer {
 public:
  struct PublishOptions {
    std::optional<int64_t> stale_threshold_ms;
  };

  SnapshotProducer(IMessagePublisher* publisher,
                   ISnapshotStorage* storage,  // nullable
                   const SnapshotProducerConfig& config);

  // Process a raw snapshot: derive status, normalize, dedup, publish, persist.
  // Returns true if the snapshot was published (not a duplicate).
  bool Publish(const domain::VenueRawSnapshot& raw,
               fob::venue::v1::VenueSnapshot* normalized_snapshot_out = nullptr,
               const PublishOptions& options = {});

  struct Stats {
    uint64_t published{0};
    uint64_t duplicates_skipped{0};
    uint64_t storage_writes{0};
    uint64_t storage_failures{0};
  };

  Stats GetStats() const;

 private:
  bool IsDuplicate(const std::string& venue_id, const std::string& symbol,
                   uint64_t sequence);

  IMessagePublisher* publisher_;
  ISnapshotStorage* storage_;
  SnapshotProducerConfig config_;
  domain::SnapshotAgeTracker age_tracker_;

  mutable std::mutex mu_;
  // Last published sequence per venue|symbol.
  std::unordered_map<std::string, uint64_t> last_sequence_;
  Stats stats_;
};

}  // namespace cex::venues::app
