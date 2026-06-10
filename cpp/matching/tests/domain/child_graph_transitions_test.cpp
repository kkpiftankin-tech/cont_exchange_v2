// ============================================================================
// child_graph_transitions_test.cpp — F-09 (T-F09-045). Hand-rolled harness.
// OCO cancel sibling (+идемпотентность), bracket resize (+идемпотентность),
// source не исполнен → без перехода (AC-F09-007/008).
// ============================================================================

#include <iostream>
#include <string>

#include "domain/child_graph_transitions.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::matching::domain;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

d::GroupLeg MakeLeg(const std::string& id, std::int64_t q_max, std::int64_t filled,
                    bool is_exit = false) {
  d::GroupLeg leg;
  leg.leg_id = id;
  leg.q_max = Decimal{q_max, 0};
  leg.filled_cum = Decimal{filled, 0};
  leg.is_exit = is_exit;
  leg.status = d::GroupLegStatus::kActive;
  return leg;
}

const d::GroupLeg* Find(const d::ComboGroupState& g, const std::string& id) {
  for (const auto& l : g.legs) if (l.leg_id == id) return &l;
  return nullptr;
}

}  // namespace

int main() {
  bool ok = true;

  // 1) OCO: A filled → B cancelled; повторный вызов — без новых переходов.
  {
    d::ComboGroupState g;
    g.parent_order_id = "grp1";
    g.legs.push_back(MakeLeg("A", 10, 5));   // исполнена
    g.legs.push_back(MakeLeg("B", 10, 0));   // активна
    g.edges.push_back(d::GroupEdge{d::GroupEdgeType::kOcoSibling, "A", "B"});

    const auto t1 = d::ApplyOCOTransitions(g);
    ok = expect(t1.size() == 1, "OCO: one transition") && ok;
    ok = expect(t1.size() == 1 && t1[0].action == "oco_cancel", "OCO: action oco_cancel") && ok;
    const auto* b = Find(g, "B");
    ok = expect(b && b->status == d::GroupLegStatus::kCancelled, "OCO: B cancelled") && ok;

    const auto t2 = d::ApplyOCOTransitions(g);
    ok = expect(t2.empty(), "OCO: idempotent re-apply (no new transitions)") && ok;
    ok = expect(Find(g, "B")->status == d::GroupLegStatus::kCancelled, "OCO: B stays cancelled") && ok;
  }

  // 2) Bracket: entry filled 6/10 → TP/SL q_max = 6; повторно — без переходов.
  {
    d::ComboGroupState g;
    g.parent_order_id = "grp2";
    g.legs.push_back(MakeLeg("E", 10, 6));            // entry, исполнено 6
    g.legs.push_back(MakeLeg("TP", 10, 0, true));     // take-profit
    g.legs.push_back(MakeLeg("SL", 10, 0, true));     // stop-loss
    g.edges.push_back(d::GroupEdge{d::GroupEdgeType::kBracketEntry, "E", "TP"});
    g.edges.push_back(d::GroupEdge{d::GroupEdgeType::kBracketEntry, "E", "SL"});

    const auto t1 = d::ResizeBracketExits(g);
    ok = expect(t1.size() == 2, "bracket: two resizes") && ok;
    ok = expect(Decimal::cmp(Find(g, "TP")->q_max, Decimal{6, 0}) == 0, "bracket: TP q_max=6") && ok;
    ok = expect(Decimal::cmp(Find(g, "SL")->q_max, Decimal{6, 0}) == 0, "bracket: SL q_max=6") && ok;

    const auto t2 = d::ResizeBracketExits(g);
    ok = expect(t2.empty(), "bracket: idempotent re-apply") && ok;
  }

  // 3) OCO source не исполнен → без перехода.
  {
    d::ComboGroupState g;
    g.parent_order_id = "grp3";
    g.legs.push_back(MakeLeg("A", 10, 0));  // не исполнена
    g.legs.push_back(MakeLeg("B", 10, 0));
    g.edges.push_back(d::GroupEdge{d::GroupEdgeType::kOcoSibling, "A", "B"});

    const auto t = d::ApplyOCOTransitions(g);
    ok = expect(t.empty(), "OCO: source not filled → no transition") && ok;
    ok = expect(Find(g, "B")->status == d::GroupLegStatus::kActive, "OCO: B stays active") && ok;
  }

  // Conditional (MVP-4.1): target waiting, условие → активация; идемпотентно.
  {
    d::ComboGroupState g;
    g.parent_order_id = "p-cond";
    g.legs.push_back(MakeLeg("S", 10, 5));  // source
    auto target = MakeLeg("C", 10, 0);
    target.status = d::GroupLegStatus::kWaitingForTrigger;
    g.legs.push_back(target);
    d::GroupEdge edge{d::GroupEdgeType::kConditional, "S", "C"};
    edge.condition =
        d::TriggerCondition{true, "BTCUSDT", d::TriggerOp::kGte, cex::common::Decimal{100, 0}};
    g.edges.push_back(edge);

    const d::ReferencePrices hit = {{"BTCUSDT", cex::common::Decimal{150, 0}}};   // 150 >= 100
    const d::ReferencePrices miss = {{"BTCUSDT", cex::common::Decimal{50, 0}}};   // 50 < 100

    const auto t_miss = d::ApplyConditionalActivations(g, miss);
    ok = expect(t_miss.empty() && Find(g, "C")->status == d::GroupLegStatus::kWaitingForTrigger,
                "conditional: not met → stays waiting") && ok;

    const auto t_hit = d::ApplyConditionalActivations(g, hit);
    ok = expect(t_hit.size() == 1 && t_hit[0].action == "conditional_activate",
                "conditional: met → activated") && ok;
    ok = expect(Find(g, "C")->status == d::GroupLegStatus::kActive, "conditional: C active") && ok;

    const auto t_again = d::ApplyConditionalActivations(g, hit);
    ok = expect(t_again.empty(), "conditional: idempotent (already active)") && ok;
  }

  if (ok) { std::cout << "child_graph_transitions_test: ALL PASSED\n"; return 0; }
  return 1;
}
