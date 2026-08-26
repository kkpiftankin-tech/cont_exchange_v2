#pragma once
// ============================================================================
// clickhouse_vector_segment_storage.hpp — F-05A (T-F05A-206). market_data infra.
//
// Персист векторных flow-сегментов в ClickHouse vector_flow_segments_history
// (ReplacingMergeTree). raw INSERT ... FORMAT JSONEachRow (паттерн
// ClickHouseLiquidityCurveStorage). Аналитика/replay — не критический путь.
// ============================================================================

#include <string>

#include <clickhouse/client.h>

#include "app/ports/i_vector_segment_storage.hpp"
#include "infra/clickhouse_storage.hpp"  // ClickHouseConfig

namespace cex::market_data::infra::clickhouse {

class ClickHouseVectorSegmentStorage final : public app::IVectorSegmentStorage {
 public:
  explicit ClickHouseVectorSegmentStorage(
      const ClickHouseConfig& config,
      const std::string& table = "vector_flow_segments_history");

  void EnsureSchema();

  void SaveSegments(const std::string& batch_id,
                    const domain::VectorizeResult& result,
                    long long event_ts_ms) override;

 private:
  ::clickhouse::Client client_;
  std::string table_;
  std::string database_;
};

}  // namespace cex::market_data::infra::clickhouse
