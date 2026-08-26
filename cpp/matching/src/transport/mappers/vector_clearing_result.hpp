#pragma once
// ============================================================================
// vector_clearing_result.hpp — F-05A (T-F05A-305, 1a). Matching transport mapper.
//
// app::VectorClearingOutcome → proto fob.marketdata.v1.VectorClearingResult
// (диагностический event для топика matching.vector_clearing). Чистый маппинг;
// денежных сайд-эффектов нет (это НЕ FillEvent — только результат/диагностика).
// ============================================================================

#include <cstdint>
#include <string>

#include "fob/marketdata/v1/vector_liquidity.pb.h"

#include "app/vector_clearing_use_case.hpp"

namespace cex::matching::transport {

fob::marketdata::v1::VectorClearingResult ToVectorClearingResult(
    const app::VectorClearingOutcome& outcome, const std::string& batch_id,
    std::int32_t decimal_scale = 12);

}  // namespace cex::matching::transport
