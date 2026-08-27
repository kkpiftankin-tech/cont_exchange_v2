#pragma once
// ============================================================================
// i_vector_clearing_result_storage.hpp — F-05A (T-F05A-305 1a persister).
// market_data app port. Персист VectorClearingResult в аналитику (ClickHouse
// vector_clearing_results). Реализация — infra::clickhouse.
// ============================================================================

#include "fob/marketdata/v1/vector_liquidity.pb.h"

namespace cex::market_data::app {

struct IVectorClearingResultStorage {
  virtual ~IVectorClearingResultStorage() = default;
  virtual void SaveResult(
      const fob::marketdata::v1::VectorClearingResult& result) = 0;
};

}  // namespace cex::market_data::app
