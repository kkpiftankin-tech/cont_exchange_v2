#pragma once
// ============================================================================
// combo_external_routing.hpp — F-09 MVP-5 (ADR-037). Matching app (pure).
//
// Разделяет ноги combo-группы на internal (→ grouped solver) и external
// (→ ExecutionIntent → venues, reuse F-12 path) и строит generic ExecutionIntent
// из external-ноги. Чистые функции; intent_id/детерминизм — на вызывающем
// (matching_loop). Не публикует и не трогает solver — только логика разбиения.
// ============================================================================

#include <string>
#include <vector>

#include "domain/multileg_vector_order.hpp"
#include "fob/execution/v1/execution.pb.h"

namespace cex::matching::app {

struct SplitLegs {
  std::vector<domain::VectorLeg> internal;  ///< → grouped solver
  std::vector<domain::VectorLeg> external;  ///< → ExecutionIntent
};

/// Разбивает ноги группы по `VectorLeg::is_external()`.
[[nodiscard]] SplitLegs SplitInternalExternal(const domain::MultiLegVectorOrder& order);

/// Строит generic ExecutionIntent из external-ноги (ADR-037). `internal_order_id`
/// = leg_id (линковка ExecutionReport → combo-нога). side — из знака target_ratio,
/// target_qty — remaining_qty(), allowed_venues — venue_preferences.
[[nodiscard]] fob::execution::v1::ExecutionIntent BuildExternalIntent(
    const std::string& parent_order_id, const std::string& batch_id,
    const std::string& intent_id, const domain::VectorLeg& leg);

}  // namespace cex::matching::app
