// ============================================================================
// auto_resolve_policy_test.cpp — F-09 MVP-7 (ADR-041). Hand-rolled harness.
// Покрытие всех guardrails + границ. Money-safety ядро.
// ============================================================================

#include <iostream>

#include "app/auto_resolve_policy.hpp"
#include "cex/common/decimal.hpp"

namespace {
using cex::common::Decimal;
namespace a = cex::order_flow::app;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

a::AutoResolveConfig Cfg() {
  a::AutoResolveConfig cfg;
  cfg.enabled = true;
  cfg.max_notional = Decimal{1000, 0};    // 1000 per-comp
  cfg.window_notional = Decimal{5000, 0}; // 5000 aggregate
  cfg.max_per_window = 3;
  cfg.min_age_ms = 10000;                 // 10s debounce
  return cfg;
}
a::AutoResolveCandidate Cand() {
  a::AutoResolveCandidate c;
  c.compensation_id = "comp-1";
  c.reason = "rejected";
  c.age_ms = 20000;                       // > min_age
  c.reversal_notional = Decimal{500, 0};  // < cap
  return c;
}
const a::AutoResolveWindow kEmpty{};

bool isReverse(const a::AutoResolveResult& r) { return r.decision == a::AutoDecision::kAutoReverse; }
}  // namespace

int main() {
  bool ok = true;

  // Happy path → auto reverse.
  {
    const auto r = a::EvaluateAutoResolve(Cand(), Cfg(), kEmpty);
    ok = expect(isReverse(r) && r.reason_code == "auto_reverse", "happy → auto_reverse") && ok;
  }
  // Disabled.
  {
    auto cfg = Cfg(); cfg.enabled = false;
    const auto r = a::EvaluateAutoResolve(Cand(), cfg, kEmpty);
    ok = expect(!isReverse(r) && r.reason_code == "disabled", "disabled → escalate") && ok;
  }
  // Non-terminal reason.
  {
    auto c = Cand(); c.reason = "weird";
    const auto r = a::EvaluateAutoResolve(c, Cfg(), kEmpty);
    ok = expect(r.reason_code == "non_terminal_reason", "non-terminal → escalate") && ok;
  }
  // Too young.
  {
    auto c = Cand(); c.age_ms = 5000;
    const auto r = a::EvaluateAutoResolve(c, Cfg(), kEmpty);
    ok = expect(r.reason_code == "too_young", "too young → escalate") && ok;
  }
  // Nothing to reverse (zero notional).
  {
    auto c = Cand(); c.reversal_notional = Decimal::zero();
    const auto r = a::EvaluateAutoResolve(c, Cfg(), kEmpty);
    ok = expect(r.reason_code == "nothing_to_reverse", "zero notional → escalate") && ok;
  }
  // Notional over cap.
  {
    auto c = Cand(); c.reversal_notional = Decimal{1500, 0};
    const auto r = a::EvaluateAutoResolve(c, Cfg(), kEmpty);
    ok = expect(r.reason_code == "notional_over_cap", "over cap → escalate") && ok;
  }
  // Boundary: notional == cap → auto (strict >).
  {
    auto c = Cand(); c.reversal_notional = Decimal{1000, 0};
    const auto r = a::EvaluateAutoResolve(c, Cfg(), kEmpty);
    ok = expect(isReverse(r), "notional == cap → auto") && ok;
  }
  // Window count exceeded (count already at max).
  {
    a::AutoResolveWindow w; w.count = 3; w.spent_notional = Decimal{0, 0};
    const auto r = a::EvaluateAutoResolve(Cand(), Cfg(), w);
    ok = expect(r.reason_code == "window_count_exceeded", "window count → escalate") && ok;
  }
  // Boundary: count+1 == max → auto.
  {
    a::AutoResolveWindow w; w.count = 2;
    const auto r = a::EvaluateAutoResolve(Cand(), Cfg(), w);
    ok = expect(isReverse(r), "count+1 == max → auto") && ok;
  }
  // Window notional circuit-breaker.
  {
    a::AutoResolveWindow w; w.count = 1; w.spent_notional = Decimal{4800, 0};
    const auto r = a::EvaluateAutoResolve(Cand(), Cfg(), w);  // 4800+500 > 5000
    ok = expect(r.reason_code == "window_notional_exceeded", "window notional → escalate") && ok;
  }
  // Boundary: spent + notional == window cap → auto.
  {
    a::AutoResolveWindow w; w.count = 1; w.spent_notional = Decimal{4500, 0};
    const auto r = a::EvaluateAutoResolve(Cand(), Cfg(), w);  // 4500+500 == 5000
    ok = expect(isReverse(r), "spent+notional == window cap → auto") && ok;
  }

  if (ok) { std::cout << "auto_resolve_policy_test: ALL PASSED\n"; return 0; }
  return 1;
}
