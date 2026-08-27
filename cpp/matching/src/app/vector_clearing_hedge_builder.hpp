#pragma once
// ============================================================================
// vector_clearing_hedge_builder.hpp — F-05A (T-F05A-305 money-path, ADR-049).
// Matching app. Чистая логика.
//
// Сошедшийся векторный клиринг → биржевой хедж (F-12): для каждого сегмента с
// x_i > 0 строим ExecutionIntent (execution.intents) против исходного external
// venue-уровня. НЕ user-проводки (сегменты — внешняя ликвидность без user_id).
// Гейтится вызывающей стороной за F05A_MONEY_ENABLED + converged-only (ADR-049).
// ============================================================================

#include <vector>

#include "fob/execution/v1/execution.pb.h"
#include "fob/marketdata/v1/vector_liquidity.pb.h"

#include "app/vector_clearing_use_case.hpp"  // VectorClearingOutcome

namespace cex::matching::app {

/// Построить hedge-intents из входа клиринга + результата. Только сегменты с
/// x_i > 0. Детерминировано: intent_id = batch_id|segment_id.
///   side: BID-сегмент → SELL (продаём в чужой bid), ASK → BUY;
///   target_qty = x_i (Decimal §9); limit_price = segment.effective_price.
std::vector<fob::execution::v1::ExecutionIntent> BuildHedgeIntents(
    const fob::marketdata::v1::VectorClearingInput& input,
    const VectorClearingOutcome& outcome);

}  // namespace cex::matching::app
