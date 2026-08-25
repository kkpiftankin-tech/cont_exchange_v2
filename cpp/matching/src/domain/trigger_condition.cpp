// ============================================================================
// trigger_condition.cpp — F-09 (MVP-4.1). См. .hpp.
// ============================================================================

#include "domain/trigger_condition.hpp"

namespace cex::matching::domain {

TriggerOp TriggerOpFromString(const std::string& op) {
  if (op == "lte") return TriggerOp::kLte;
  if (op == "gt") return TriggerOp::kGt;
  if (op == "lt") return TriggerOp::kLt;
  return TriggerOp::kGte;  // "gte" / неизвестное
}

bool EvaluateTrigger(const TriggerCondition& cond, const ReferencePrices& prices) {
  if (!cond.present) return true;  // безусловно

  const auto it = prices.find(cond.symbol);
  if (it == prices.end()) return false;  // нет цены → нельзя оценить
  const int c = cex::common::Decimal::cmp(it->second, cond.threshold);

  switch (cond.op) {
    case TriggerOp::kGte: return c >= 0;
    case TriggerOp::kLte: return c <= 0;
    case TriggerOp::kGt: return c > 0;
    case TriggerOp::kLt: return c < 0;
  }
  return false;
}

}  // namespace cex::matching::domain
