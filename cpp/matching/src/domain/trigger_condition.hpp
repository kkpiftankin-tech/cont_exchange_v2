#pragma once
// ============================================================================
// trigger_condition.hpp — F-09 (MVP-4.1). Matching domain (pure).
//
// Триггерные условия для conditional_links (COMBO_TYPE_CONDITIONAL / kConditional
// рёбра): активация target-ноги, когда рыночная цена символа пересекает порог.
//
// Схема condition (JSONB в conditional_links.condition; NULL = безусловно):
//   {"op": "gte"|"lte"|"gt"|"lt", "symbol": "<sym>", "price": "<decimal>"}
// Поля извлекаются loader'ом через PG-операторы (->>'op' и т.п.), без C++ JSON.
// ============================================================================

#include <string>

#include "cex/common/decimal.hpp"
#include "domain/multileg_feasible_caps.hpp"  // ReferencePrices

namespace cex::matching::domain {

enum class TriggerOp { kGte, kLte, kGt, kLt };

struct TriggerCondition {
  bool present{false};  ///< false = безусловно (condition IS NULL)
  std::string symbol;
  TriggerOp op{TriggerOp::kGte};
  cex::common::Decimal threshold{};
};

/// Парсит строковый оператор (для loader). Неизвестный → kGte.
[[nodiscard]] TriggerOp TriggerOpFromString(const std::string& op);

/// true, если условие отсутствует (безусловно) ИЛИ рыночная цена символа
/// удовлетворяет ему. Нет цены по символу → false (нельзя оценить → не триггерим).
[[nodiscard]] bool EvaluateTrigger(const TriggerCondition& cond, const ReferencePrices& prices);

}  // namespace cex::matching::domain
