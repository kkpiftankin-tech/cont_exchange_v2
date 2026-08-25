#pragma once
// ============================================================================
// auto_resolve_policy.hpp — F-09 MVP-7 (ADR-041). Pure guardrail-функция.
//
// Решает, можно ли АВТОМАТИЧЕСКИ разрешить pending-компенсацию через
// reverse_internal, или оставить оператору (escalate). Money-safety ядро:
// чистая функция (без IO/времени/денег), детерминированная, тестируемая.
// Реальный money-путь — тот же ResolveCompensationUseCase (ADR-040), вызывается
// loop'ом только при decision=kAutoReverse. Авто-accept/retry — запрещены (ADR-041).
// ============================================================================

#include <string>

#include "cex/common/decimal.hpp"

namespace cex::order_flow::app {

/// Лимиты авто-резолва (из env). enabled=false → всё escalate (default OFF).
struct AutoResolveConfig {
  bool enabled{false};
  cex::common::Decimal max_notional{};      ///< потолок notional на одну компенсацию
  cex::common::Decimal window_notional{};   ///< агрегатный потолок в окне (circuit-breaker)
  int max_per_window{0};                    ///< макс. число авто-резолвов в окне
  std::int64_t min_age_ms{0};               ///< debounce: компенсация должна «отстояться»
};

/// Кандидат на авто-резолв (loop наполняет из ListPending + загруженных ног).
struct AutoResolveCandidate {
  std::string compensation_id;
  std::string reason;                          ///< rejected | timeout | cancelled
  std::int64_t age_ms{0};                      ///< now - created_at
  cex::common::Decimal reversal_notional{};    ///< оценка Σ filled_cum·mid(p_low,p_high)
};

/// Текущее состояние скользящего окна (накопитель loop'а).
struct AutoResolveWindow {
  cex::common::Decimal spent_notional{};
  int count{0};
};

enum class AutoDecision { kAutoReverse, kEscalate };

struct AutoResolveResult {
  AutoDecision decision{AutoDecision::kEscalate};
  std::string reason_code;  ///< почему (audit): disabled/non_terminal/too_young/...
};

/// Чистое решение. kAutoReverse ТОЛЬКО если все guardrails выполнены; иначе
/// kEscalate с reason_code (компенсация остаётся pending оператору — fail-safe).
[[nodiscard]] AutoResolveResult EvaluateAutoResolve(const AutoResolveCandidate& c,
                                                    const AutoResolveConfig& cfg,
                                                    const AutoResolveWindow& window);

}  // namespace cex::order_flow::app
