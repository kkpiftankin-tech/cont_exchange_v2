#pragma once
// ============================================================================
// clickhouse_vector_clearing_storage.hpp — F-05A (T-F05A-305 1a persister).
// market_data infra. Персист VectorClearingResult в ClickHouse
// vector_clearing_results (ReplacingMergeTree). raw INSERT ... FORMAT JSONEachRow.
// ============================================================================

#include <string>

#include <clickhouse/client.h>

#include "app/ports/i_vector_clearing_result_storage.hpp"
#include "infra/clickhouse_storage.hpp"  // ClickHouseConfig

namespace cex::market_data::infra::clickhouse {

class ClickHouseVectorClearingStorage final
    : public app::IVectorClearingResultStorage {
 public:
  explicit ClickHouseVectorClearingStorage(
      const ClickHouseConfig& config,
      const std::string& table = "vector_clearing_results");

  void EnsureSchema();

  void SaveResult(
      const fob::marketdata::v1::VectorClearingResult& result) override;

 private:
  ::clickhouse::Client client_;
  std::string table_;
  std::string database_;
};

}  // namespace cex::market_data::infra::clickhouse
