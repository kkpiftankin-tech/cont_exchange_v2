// ============================================================================
// solve_grouped_batch_use_case_test.cpp — F-09 (T-F09-045). Hand-rolled.
// Батч из нескольких групп через реальный grouped solver + детерминизм replay.
// ============================================================================

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "app/solve_grouped_batch_use_case.hpp"
#include "domain/grouped_solver_bisection.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::matching::domain;
namespace a = cex::matching::app;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

d::VectorLeg MakeLeg(const std::string& sym, std::int64_t ratio, std::int64_t q_max,
                     std::optional<std::int64_t> q_liq = std::nullopt) {
  d::VectorLeg leg;
  leg.instrument_symbol = sym;
  leg.target_ratio = Decimal{ratio, 0};
  leg.q_rate = Decimal{100, 0};
  leg.q_max = Decimal{q_max, 0};
  leg.filled_cum = Decimal::zero();
  if (q_liq.has_value()) leg.q_liq = Decimal{*q_liq, 0};
  return leg;
}

// Пара BTC(+1)/ETH(-1); ETH ограничена ликвидностью eth_liq.
d::MultiLegVectorOrder MakeGroup(const std::string& id, d::GroupAtomicityPolicy policy,
                                 std::int64_t eth_liq) {
  d::MultiLegVectorOrder g;
  g.parent_order_id = id;
  g.atomicity_policy = policy;
  g.legs.push_back(MakeLeg("BTCUSDT", 1, 10));
  g.legs.push_back(MakeLeg("ETHUSDT", -1, 10, eth_liq));
  return g;
}

const a::GroupBatchResult* Find(const std::vector<a::GroupBatchResult>& rs, const std::string& id) {
  for (const auto& r : rs) if (r.parent_order_id == id) return &r;
  return nullptr;
}

}  // namespace

int main() {
  bool ok = true;
  d::GroupedSolverBisection solver;
  a::SolveGroupedBatchUseCase uc{solver};
  const d::ReferencePrices prices;

  const std::vector<d::MultiLegVectorOrder> groups = {
      MakeGroup("g-full", d::GroupAtomicityPolicy::kStrictAtomic, 10),    // оба liquid → full
      MakeGroup("g-block", d::GroupAtomicityPolicy::kStrictAtomic, 4),    // ETH illiquid → blocked
      MakeGroup("g-scaled", d::GroupAtomicityPolicy::kScalableAtomic, 6), // α=0.6
  };

  const auto results = uc.Execute(groups, prices);
  ok = expect(results.size() == 3, "three group results, in order") && ok;
  ok = expect(results.size() == 3 && results[0].parent_order_id == "g-full" &&
                  results[2].parent_order_id == "g-scaled",
              "result order preserved") && ok;

  // g-full: strict, both liquid → fully executed, оба исполнены по 10.
  if (const auto* r = Find(results, "g-full")) {
    ok = expect(r->solve.status == d::GroupExecStatus::kFullyExecuted, "g-full: fully executed") && ok;
    ok = expect(r->solve.leg_execs.size() == 2, "g-full: 2 leg fills") && ok;
  } else {
    ok = expect(false, "g-full present") && ok;
  }

  // g-block: strict, ETH illiquid → blocked, без fills.
  if (const auto* r = Find(results, "g-block")) {
    ok = expect(r->solve.status == d::GroupExecStatus::kBlocked, "g-block: blocked") && ok;
    ok = expect(r->solve.leg_execs.empty(), "g-block: no fills") && ok;
  } else {
    ok = expect(false, "g-block present") && ok;
  }

  // g-scaled: scalable, α=0.6.
  if (const auto* r = Find(results, "g-scaled")) {
    ok = expect(r->solve.status == d::GroupExecStatus::kScaled, "g-scaled: scaled") && ok;
    ok = expect(Decimal::cmp(r->solve.execution_scale, Decimal{6, 1}) == 0, "g-scaled: α=0.6") && ok;
  } else {
    ok = expect(false, "g-scaled present") && ok;
  }

  // DuplicateGroupEventNoDuplicateFills: повторный solve того же batch → идентично.
  {
    const auto again = uc.Execute(groups, prices);
    bool same = again.size() == results.size();
    for (std::size_t i = 0; same && i < results.size(); ++i) {
      same = again[i].parent_order_id == results[i].parent_order_id &&
             again[i].solve.status == results[i].solve.status &&
             again[i].solve.leg_execs.size() == results[i].solve.leg_execs.size() &&
             Decimal::cmp(again[i].solve.execution_scale, results[i].solve.execution_scale) == 0;
    }
    ok = expect(same, "replay batch: identical results (idempotent, no dup fills)") && ok;
  }

  if (ok) { std::cout << "solve_grouped_batch_use_case_test: ALL PASSED\n"; return 0; }
  return 1;
}
