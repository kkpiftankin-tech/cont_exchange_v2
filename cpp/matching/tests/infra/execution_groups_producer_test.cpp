// ============================================================================
// execution_groups_producer_test.cpp — F-09 (T-F09-046). Hand-rolled harness.
// Тестирует чистый маппинг BuildExecutionGroup (без Kafka): GroupStatus,
// key=parentOrderId, leg_results (qty/price/notional), blocked→пусто.
// ============================================================================

#include <iostream>
#include <optional>
#include <string>

#include "domain/grouped_solver_bisection.hpp"
#include "infra/execution_groups_producer.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::matching::domain;
namespace infra = cex::matching::infra;
namespace mv1 = fob::matching::v1;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

d::VectorLeg MakeLeg(const std::string& leg_id, const std::string& sym, std::int64_t ratio,
                     std::int64_t q_max, std::optional<std::int64_t> q_liq = std::nullopt) {
  d::VectorLeg leg;
  leg.leg_id = leg_id;
  leg.instrument_symbol = sym;
  leg.target_ratio = Decimal{ratio, 0};
  leg.q_rate = Decimal{100, 0};
  leg.q_max = Decimal{q_max, 0};
  leg.filled_cum = Decimal::zero();
  if (q_liq.has_value()) leg.q_liq = Decimal{*q_liq, 0};
  return leg;
}

d::MultiLegVectorOrder MakePair(const std::string& id, d::GroupAtomicityPolicy policy,
                                std::int64_t eth_liq) {
  d::MultiLegVectorOrder g;
  g.parent_order_id = id;
  g.user_id = "user-x";
  g.atomicity_policy = policy;
  g.legs.push_back(MakeLeg("leg-btc", "BTCUSDT", 1, 10));
  g.legs.push_back(MakeLeg("leg-eth", "ETHUSDT", -1, 10, eth_liq));
  return g;
}

infra::ExecutionGroupRecord MakeRecord(const d::MultiLegVectorOrder& order) {
  d::GroupedSolverBisection solver;
  d::GroupedSolveInput in;
  in.order = order;
  in.feasible_caps = d::ComputeFeasibleCaps(order, {});
  infra::ExecutionGroupRecord rec;
  rec.execution_group_id = "eg-1";
  rec.batch_id = "batch-1";
  rec.order = order;
  rec.result = solver.Solve(in);
  rec.reference_prices = {{"BTCUSDT", Decimal{100, 0}}, {"ETHUSDT", Decimal{50, 0}}};
  return rec;
}

const mv1::LegResult* Find(const mv1::ExecutionGroup& eg, const std::string& leg_id) {
  for (const auto& lr : eg.leg_results()) if (lr.leg_id() == leg_id) return &lr;
  return nullptr;
}

}  // namespace

int main() {
  bool ok = true;

  // 1) scalable (ETH cap=6) → PARTIAL, key=parent, 2 leg_results, qty/price/notional.
  {
    const auto rec = MakeRecord(MakePair("parent-1", d::GroupAtomicityPolicy::kScalableAtomic, 6));
    const auto eg = infra::BuildExecutionGroup(rec);
    ok = expect(eg.meta().partition_key() == "parent-1", "key = parentOrderId (ADR-033)") && ok;
    ok = expect(eg.parent_order_id() == "parent-1", "parent_order_id set") && ok;
    ok = expect(eg.execution_group_id() == "eg-1", "execution_group_id set") && ok;
    ok = expect(eg.group_status() == mv1::GROUP_STATUS_PARTIAL, "scalable → PARTIAL") && ok;
    ok = expect(eg.leg_results_size() == 2, "two leg_results") && ok;
    const auto* btc = Find(eg, "leg-btc");
    // e_BTC=6, price 100 → notional 600.
    ok = expect(btc && Decimal::cmp(Decimal::from_proto(btc->exec_qty()), Decimal{6, 0}) == 0,
                "BTC exec_qty=6") && ok;
    ok = expect(btc && Decimal::cmp(Decimal::from_proto(btc->exec_notional()), Decimal{600, 0}) == 0,
                "BTC exec_notional=600") && ok;
    ok = expect(eg.execution_mode() == fob::orders::v1::EXECUTION_MODE_MULTILEG_VECTOR_SOLVER,
                "execution_mode = multileg_vector_solver") && ok;
    // T-F09-062: user_id + per-leg instrument_symbol/side для ledger.
    ok = expect(eg.user_id() == "user-x", "user_id set (T-F09-062)") && ok;
    const auto* eth = Find(eg, "leg-eth");
    ok = expect(btc && btc->instrument_symbol() == "BTCUSDT", "BTC leg instrument_symbol") && ok;
    ok = expect(btc && btc->side() == fob::common::v1::SIDE_BUY, "BTC leg side BUY (ratio +1)") && ok;
    ok = expect(eth && eth->side() == fob::common::v1::SIDE_SELL, "ETH leg side SELL (ratio -1)") && ok;
  }

  // 2) strict, ETH illiquid (cap=4) → CANCELLED_BY_ATOMICITY, leg_results пусто.
  {
    const auto rec = MakeRecord(MakePair("parent-2", d::GroupAtomicityPolicy::kStrictAtomic, 4));
    const auto eg = infra::BuildExecutionGroup(rec);
    ok = expect(eg.group_status() == mv1::GROUP_STATUS_CANCELLED_BY_ATOMICITY,
                "strict blocked → CANCELLED_BY_ATOMICITY") && ok;
    ok = expect(eg.leg_results_size() == 0, "blocked → leg_results empty") && ok;
    ok = expect(eg.fallback_action() == "scale_down", "fallback_action set") && ok;
  }

  if (ok) { std::cout << "execution_groups_producer_test: ALL PASSED\n"; return 0; }
  return 1;
}
