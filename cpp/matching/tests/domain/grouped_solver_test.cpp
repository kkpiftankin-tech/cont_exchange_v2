// ============================================================================
// grouped_solver_test.cpp — F-09 (T-F09-044). Hand-rolled harness.
// strict_atomic (blocked), scalable_atomic (α, ratio preserved), best_effort
// (degraded), replay determinism (AC-F09-010), scalable below α_min.
// ============================================================================

#include <iostream>
#include <optional>
#include <string>

#include "domain/grouped_solver_bisection.hpp"
#include "domain/multileg_feasible_caps.hpp"
#include "domain/multileg_vector_order.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::matching::domain;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

// Нога: символ, знаковый ratio, q_max, опциональный q_liq.
d::VectorLeg MakeLeg(const std::string& sym, std::int64_t ratio, std::int64_t q_max,
                     std::optional<std::int64_t> q_liq = std::nullopt) {
  d::VectorLeg leg;
  leg.instrument_symbol = sym;
  leg.target_ratio = Decimal{ratio, 0};
  leg.p_low = Decimal{100, 0};
  leg.p_high = Decimal{200, 0};
  leg.q_rate = Decimal{100, 0};  // не ограничивает
  leg.q_max = Decimal{q_max, 0};
  leg.filled_cum = Decimal::zero();
  if (q_liq.has_value()) leg.q_liq = Decimal{*q_liq, 0};
  return leg;
}

// Pair BTC(+1) / ETH(-1): BTC cap=10, ETH cap=eth_liq (ликвидность ограничивает).
d::GroupedSolveInput MakePairInput(d::GroupAtomicityPolicy policy, std::int64_t eth_liq,
                                   Decimal min_scale = Decimal::zero()) {
  d::MultiLegVectorOrder order;
  order.parent_order_id = "g1";
  order.atomicity_policy = policy;
  order.fallback_policy = d::GroupFallbackPolicy::kScaleDown;
  order.min_execution_scale = min_scale;
  order.legs.push_back(MakeLeg("BTCUSDT", 1, 10));
  order.legs.push_back(MakeLeg("ETHUSDT", -1, 10, eth_liq));

  d::GroupedSolveInput in;
  in.order = order;
  in.feasible_caps = d::ComputeFeasibleCaps(order, {});
  return in;
}

const d::LegExec* Find(const d::GroupedSolveResult& r, const std::string& sym) {
  for (const auto& e : r.leg_execs) if (e.instrument_symbol == sym) return &e;
  return nullptr;
}

bool HasViolation(const d::GroupedSolveResult& r, const std::string& v) {
  for (const auto& x : r.violated_constraints) if (x == v) return true;
  return false;
}

}  // namespace

int main() {
  bool ok = true;
  d::GroupedSolverBisection solver;

  // 1) strict_atomic, ETH illiquid (cap=4 < remaining 10) → blocked, scale=0, no fills.
  {
    const auto r = solver.Solve(MakePairInput(d::GroupAtomicityPolicy::kStrictAtomic, 4));
    ok = expect(r.status == d::GroupExecStatus::kBlocked, "strict: blocked") && ok;
    ok = expect(Decimal::cmp(r.execution_scale, Decimal::zero()) == 0, "strict: scale=0") && ok;
    ok = expect(r.leg_execs.empty(), "strict: no fills (orphanLegs=0)") && ok;
    ok = expect(r.fallback_action == "scale_down", "strict: fallback action set") && ok;
  }

  // 2) scalable_atomic, ETH cap=4 → α=0.4, e_BTC=e_ETH=4 (ratio preserved, dev=0).
  {
    const auto r = solver.Solve(MakePairInput(d::GroupAtomicityPolicy::kScalableAtomic, 4));
    ok = expect(r.status == d::GroupExecStatus::kScaled, "scalable: scaled") && ok;
    ok = expect(Decimal::cmp(r.execution_scale, Decimal{4, 1}) == 0, "scalable: α=0.4") && ok;
    const auto* btc = Find(r, "BTCUSDT");
    const auto* eth = Find(r, "ETHUSDT");
    ok = expect(btc && Decimal::cmp(btc->executed_qty, Decimal{4, 0}) == 0, "scalable: BTC=4") && ok;
    ok = expect(eth && Decimal::cmp(eth->executed_qty, Decimal{4, 0}) == 0, "scalable: ETH=4") && ok;
    // ratio preserved: оба исполнены поровну (deviation 0 ≤ max bps).
    ok = expect(btc && eth && Decimal::cmp(btc->executed_qty, eth->executed_qty) == 0,
                "scalable: ratio preserved (dev=0)") && ok;
  }

  // 3) best_effort, ETH cap=4 → BTC=10, ETH=4, ratio_deviation, degraded.
  {
    const auto r = solver.Solve(MakePairInput(d::GroupAtomicityPolicy::kBestEffort, 4));
    ok = expect(r.status == d::GroupExecStatus::kDegraded, "best_effort: degraded") && ok;
    ok = expect(HasViolation(r, "ratio_deviation"), "best_effort: ratio_deviation recorded") && ok;
    const auto* btc = Find(r, "BTCUSDT");
    const auto* eth = Find(r, "ETHUSDT");
    ok = expect(btc && Decimal::cmp(btc->executed_qty, Decimal{10, 0}) == 0, "best_effort: BTC=10") && ok;
    ok = expect(eth && Decimal::cmp(eth->executed_qty, Decimal{4, 0}) == 0, "best_effort: ETH=4") && ok;
  }

  // 4) Replay determinism: тот же вход дважды → идентичный результат (AC-F09-010).
  {
    const auto a = solver.Solve(MakePairInput(d::GroupAtomicityPolicy::kScalableAtomic, 7));
    const auto b = solver.Solve(MakePairInput(d::GroupAtomicityPolicy::kScalableAtomic, 7));
    bool same = a.status == b.status && a.leg_execs.size() == b.leg_execs.size() &&
                Decimal::cmp(a.execution_scale, b.execution_scale) == 0;
    for (std::size_t i = 0; same && i < a.leg_execs.size(); ++i) {
      same = a.leg_execs[i].instrument_symbol == b.leg_execs[i].instrument_symbol &&
             Decimal::cmp(a.leg_execs[i].executed_qty, b.leg_execs[i].executed_qty) == 0;
    }
    ok = expect(same, "replay: identical result for identical input") && ok;
  }

  // 5) scalable_atomic, α=0.4 < min_execution_scale=0.5 → blocked.
  {
    const auto r = solver.Solve(
        MakePairInput(d::GroupAtomicityPolicy::kScalableAtomic, 4, Decimal{5, 1}));
    ok = expect(r.status == d::GroupExecStatus::kBlocked, "scalable<min: blocked") && ok;
    ok = expect(r.leg_execs.empty(), "scalable<min: no fills") && ok;
    ok = expect(Decimal::cmp(r.execution_scale, Decimal::zero()) == 0, "scalable<min: scale=0") && ok;
  }

  if (ok) { std::cout << "grouped_solver_test: ALL PASSED\n"; return 0; }
  return 1;
}
