#pragma once

#include <optional>
#include <string>
#include <vector>

#include "domain/entities/market_data_snapshot.hpp"

namespace cex::market_data::domain {

class ISnapshotStorage {
 public:
  virtual ~ISnapshotStorage() = default;

  virtual bool SaveSnapshot(const MarketDataSnapshot& snap) = 0;

  virtual std::optional<MarketDataSnapshot> GetLatestSnapshot(
      const std::string& asset) const = 0;

  virtual std::vector<MarketDataSnapshot> GetSnapshotHistory(
      const std::string& asset,
      Timestamp from,
      Timestamp to,
      uint32_t limit) const = 0;

  // Суммарный объём торгов за последние window_sec секунд из ClickHouse fills.
  virtual common::Decimal GetVolume24h(const std::string& asset,
                                       uint32_t window_sec = 86400) const = 0;
};

class IEffectiveSpreadStorage {
 public:
  virtual ~IEffectiveSpreadStorage() = default;

  virtual bool SaveRecord(const EffectiveSpreadRecord& record) = 0;

  virtual std::vector<EffectiveSpreadRecord> GetSpreadHistory(
      const std::string& asset,
      Timestamp from,
      Timestamp to,
      uint32_t limit) const = 0;
};

}  // namespace cex::market_data::domain
