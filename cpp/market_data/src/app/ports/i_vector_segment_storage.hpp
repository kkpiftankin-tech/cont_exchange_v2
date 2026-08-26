#pragma once
// ============================================================================
// i_vector_segment_storage.hpp — F-05A (T-F05A-206). market_data app port.
//
// Порт персистенса векторных flow-сегментов в аналитику (ClickHouse
// vector_flow_segments_history). Реализация — infra::clickhouse.
// ============================================================================

#include <string>

#include "domain/vectorize.hpp"

namespace cex::market_data::app {

struct IVectorSegmentStorage {
  virtual ~IVectorSegmentStorage() = default;
  virtual void SaveSegments(const std::string& batch_id,
                            const domain::VectorizeResult& result,
                            long long event_ts_ms) = 0;
};

}  // namespace cex::market_data::app
