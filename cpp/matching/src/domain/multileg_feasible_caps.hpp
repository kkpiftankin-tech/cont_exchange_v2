#pragma once
// ============================================================================
// multileg_feasible_caps.hpp — F-09 (T-F09-043). Matching domain (pure).
//
// Расчёт feasible caps по каждой ноге группы:
//   Q_feasible = min(Q_remaining, Q_rate, Q_liq, Q_risk, Q_venue).
// Чистая, детерминированная функция (AC-F09-010): без IO / Kafka / DB.
// ============================================================================

#include <string>
#include <unordered_map>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/multileg_vector_order.hpp"

namespace cex::matching::domain {

/// Референс-цены по символу (quote per base). В MVP-2 зарезервированы для
/// будущей конвертации risk-notional → base qty; см. ComputeFeasibleCaps.
using ReferencePrices = std::unordered_map<std::string, cex::common::Decimal>;

/// Feasible cap по каждой ноге. Детерминирована относительно входа.
[[nodiscard]] std::vector<FeasibleCap> ComputeFeasibleCaps(
    const MultiLegVectorOrder& order, const ReferencePrices& reference_prices);

}  // namespace cex::matching::domain
