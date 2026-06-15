// ============================================================================
// auto_resolve_policy.cpp — F-09 MVP-7 (ADR-041). См. .hpp.
// ============================================================================

#include "app/auto_resolve_policy.hpp"

namespace cex::order_flow::app {

namespace {
using cex::common::Decimal;

AutoResolveResult Escalate(const char* code) {
  return AutoResolveResult{AutoDecision::kEscalate, code};
}
bool IsTerminalReason(const std::string& r) {
  return r == "rejected" || r == "timeout" || r == "cancelled";
}
}  // namespace

AutoResolveResult EvaluateAutoResolve(const AutoResolveCandidate& c,
                                      const AutoResolveConfig& cfg,
                                      const AutoResolveWindow& window) {
  // 1. Включённость (default OFF, ADR-041 §4).
  if (!cfg.enabled) return Escalate("disabled");

  // 2. Терминальный провал внешней ноги.
  if (!IsTerminalReason(c.reason)) return Escalate("non_terminal_reason");

  // 3. Debounce — компенсация должна «отстояться» (поздние fill / транзиент).
  if (c.age_ms < cfg.min_age_ms) return Escalate("too_young");

  // 4. Есть что разворачивать (нулевая внутренняя экспозиция → оператору).
  if (Decimal::cmp(c.reversal_notional, Decimal::zero()) <= 0) {
    return Escalate("nothing_to_reverse");
  }

  // 5. Потолок notional на одну компенсацию.
  if (Decimal::cmp(c.reversal_notional, cfg.max_notional) > 0) {
    return Escalate("notional_over_cap");
  }

  // 6. Оконный потолок по числу авто-резолвов.
  if (window.count + 1 > cfg.max_per_window) {
    return Escalate("window_count_exceeded");
  }

  // 7. Circuit-breaker: агрегатный notional в окне (ядро money-риска ADR-039).
  if (Decimal::cmp(Decimal::add(window.spent_notional, c.reversal_notional),
                   cfg.window_notional) > 0) {
    return Escalate("window_notional_exceeded");
  }

  return AutoResolveResult{AutoDecision::kAutoReverse, "auto_reverse"};
}

}  // namespace cex::order_flow::app
