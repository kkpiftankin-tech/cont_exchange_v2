#pragma once
// ============================================================================
// vectorized_liquidity.hpp — F-05A (T-F05A-205). market_data transport mapper.
//
// domain::VectorizeResult → proto fob.marketdata.v1.VectorizedLiquiditySnapshot
// (payload топика marketdata.vectorized). Чистый маппинг: AssetBasis + сегменты
// (w → Decimal[], p_low/p_high/d_hl/q_rate/q_max → Decimal, side → enum). Деньги —
// Decimal.to_proto() (§9); w (double) квантуется в Decimal на границе.
// ============================================================================

#include <cstdint>
#include <string>

#include "fob/marketdata/v1/vector_liquidity.pb.h"

#include "domain/vectorize.hpp"

namespace cex::market_data::transport {

fob::marketdata::v1::VectorizedLiquiditySnapshot ToVectorizedSnapshot(
    const domain::VectorizeResult& result, const std::string& batch_id,
    std::int64_t event_ts_ms = 0, std::int32_t decimal_scale = 12);

}  // namespace cex::market_data::transport
