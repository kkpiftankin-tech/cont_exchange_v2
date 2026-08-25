#pragma once

#include <string>

#include "domain/ports/i_snapshot_storage.hpp"
#include "infra/clickhouse_storage.hpp"

namespace cex::market_data::infra::clickhouse {

// Реализует ISnapshotStorage + IEffectiveSpreadStorage через ClickHouse HTTP API.
// Таблицы: marketdata_snapshots, effective_spreads (DDL: docs/07-data/).
class ChSnapshotStorage final : public domain::ISnapshotStorage,
                                 public domain::IEffectiveSpreadStorage {
 public:
  explicit ChSnapshotStorage(ClickHouseConfig cfg);

  // Создаёт таблицы если не существуют.
  bool EnsureSchema();

  // ISnapshotStorage
  bool SaveSnapshot(const domain::MarketDataSnapshot& snap) override;
  std::optional<domain::MarketDataSnapshot> GetLatestSnapshot(
      const std::string& asset) const override;
  common::Decimal GetVolume24h(const std::string& asset,
                               uint32_t window_sec = 86400) const override;

  std::vector<domain::MarketDataSnapshot> GetSnapshotHistory(
      const std::string& asset,
      domain::Timestamp from,
      domain::Timestamp to,
      uint32_t limit) const override;

  // IEffectiveSpreadStorage
  bool SaveRecord(const domain::EffectiveSpreadRecord& record) override;
  std::vector<domain::EffectiveSpreadRecord> GetSpreadHistory(
      const std::string& asset,
      domain::Timestamp from,
      domain::Timestamp to,
      uint32_t limit) const override;

 private:
  bool ExecInsert(const std::string& query);
  bool ExecSelect(const std::string& query, std::string* out) const;

  // Парсит одну TSV-строку от ClickHouse в MarketDataSnapshot.
  // Формат колонок: snapshot_id, asset, mid, best_bid, best_ask, spread, spread_bps,
  //                 volume_24h, clear_price, executed_rate, source, stale, batch_id, timestamp
  static std::optional<domain::MarketDataSnapshot> ParseSnapshotRow(const std::string& row);

  ClickHouseConfig cfg_;
};

}  // namespace cex::market_data::infra::clickhouse
