// ============================================================================
// trigger_condition_test.cpp — F-09 (MVP-4.1). Hand-rolled harness.
// gte/lte/gt/lt, unconditional, missing price, op parsing.
// ============================================================================

#include <iostream>

#include "domain/trigger_condition.hpp"

namespace {
using cex::common::Decimal;
namespace d = cex::matching::domain;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

d::TriggerCondition Cond(d::TriggerOp op, std::int64_t threshold) {
  return d::TriggerCondition{true, "BTCUSDT", op, Decimal{threshold, 0}};
}
}  // namespace

int main() {
  bool ok = true;
  const d::ReferencePrices p150 = {{"BTCUSDT", Decimal{150, 0}}};
  const d::ReferencePrices p50 = {{"BTCUSDT", Decimal{50, 0}}};
  const d::ReferencePrices p100 = {{"BTCUSDT", Decimal{100, 0}}};

  // 1) Безусловно (present=false) → true.
  ok = expect(d::EvaluateTrigger(d::TriggerCondition{}, p150), "unconditional → true") && ok;

  // 2) gte.
  ok = expect(d::EvaluateTrigger(Cond(d::TriggerOp::kGte, 100), p150), "150 >= 100 → true") && ok;
  ok = expect(!d::EvaluateTrigger(Cond(d::TriggerOp::kGte, 100), p50), "50 >= 100 → false") && ok;
  ok = expect(d::EvaluateTrigger(Cond(d::TriggerOp::kGte, 100), p100), "100 >= 100 → true") && ok;

  // 3) lte / gt / lt.
  ok = expect(d::EvaluateTrigger(Cond(d::TriggerOp::kLte, 100), p50), "50 <= 100 → true") && ok;
  ok = expect(!d::EvaluateTrigger(Cond(d::TriggerOp::kGt, 100), p100), "100 > 100 → false") && ok;
  ok = expect(d::EvaluateTrigger(Cond(d::TriggerOp::kGt, 100), p150), "150 > 100 → true") && ok;
  ok = expect(d::EvaluateTrigger(Cond(d::TriggerOp::kLt, 100), p50), "50 < 100 → true") && ok;

  // 4) Нет цены по символу → false.
  ok = expect(!d::EvaluateTrigger(Cond(d::TriggerOp::kGte, 100), d::ReferencePrices{}),
              "no price → false") && ok;

  // 5) Парсинг оператора.
  ok = expect(d::TriggerOpFromString("lte") == d::TriggerOp::kLte, "parse lte") && ok;
  ok = expect(d::TriggerOpFromString("gt") == d::TriggerOp::kGt, "parse gt") && ok;
  ok = expect(d::TriggerOpFromString("xyz") == d::TriggerOp::kGte, "parse unknown → gte") && ok;

  if (ok) { std::cout << "trigger_condition_test: ALL PASSED\n"; return 0; }
  return 1;
}
