#pragma once

#include <string>

#include "app/snapshot_producer.hpp"
#include "fob/venue/v1/venue.pb.h"

namespace cex::venues::infra {

// F11-NORM-5: ClickHouse configuration for venue snapshot storage.
struct SnapshotClickHouseConfig {
  std::string url{"http://clickhouse:8123"};
  std::string database{"backtest"};
  std::string table{"venue_snapshots"};
  std::string user{"default"};
  std::string password{};
  long timeout_ms{3000};
  int retention_days{90};
};

// F11-NORM-5: ClickHouse writer for venue snapshots.
//
// Implements ISnapshotStorage to persist normalized VenueSnapshot
// directly from the venues service, indexed by venue_id/symbol/event_time_ms.
class SnapshotClickHouseWriter final : public app::ISnapshotStorage {
 public:
  explicit SnapshotClickHouseWriter(SnapshotClickHouseConfig cfg);

  bool EnsureSchema();

  // ISnapshotStorage
  bool SaveSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) override;

 private:
  bool ExecQuery(const std::string& query, const std::string& body = "");
  std::string FullTableName() const;

  SnapshotClickHouseConfig cfg_;
};

}  // namespace cex::venues::infra
