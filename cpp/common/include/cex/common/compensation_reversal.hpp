#pragma once
// ============================================================================
// compensation_reversal.hpp — F-09 MVP-6 (ADR-039, перенесён в common ADR-040 §4).
//
// Считает реверсивные ордера для разворота внутренней экспозиции combo, когда
// внешняя нога провалилась (operator action reverse_internal). Для каждой
// исполненной внутренней ноги — ордер ПРОТИВОПОЛОЖНОЙ стороны на filled-объём.
// Чистая функция (без IO/ордеров/денег). Размещена в cex::common, чтобы её мог
// использовать и matching, и order_flow (фактическое размещение FlowOrder через
// order_flow pipeline с operator-auth — T-F09-067).
// ============================================================================

#include <string>
#include <vector>

#include "cex/common/decimal.hpp"

namespace cex::common {

struct ReversalLeg {
  std::string instrument_symbol;
  bool is_buy{true};  ///< исходная сторона ноги
  cex::common::Decimal filled_qty{};
};

struct ReversalOrder {
  std::string instrument_symbol;
  bool is_buy{true};  ///< ПРОТИВОПОЛОЖНАЯ исходной (разворот)
  cex::common::Decimal qty{};
};

/// Для каждой ноги с filled_qty>0 — реверсивный ордер обратной стороны на этот
/// объём. Ноги с нулевым fill пропускаются (разворачивать нечего).
[[nodiscard]] std::vector<ReversalOrder> ComputeReversals(
    const std::vector<ReversalLeg>& internal_legs);

}  // namespace cex::common
