// ============================================================================
// child_graph_transitions.cpp — F-09 (T-F09-045). См. .hpp.
// ============================================================================

#include "domain/child_graph_transitions.hpp"

namespace cex::matching::domain {

namespace {

using cex::common::Decimal;

bool IsFilled(const GroupLeg& leg) {
  return leg.status == GroupLegStatus::kFilled ||
         Decimal::cmp(leg.filled_cum, Decimal::zero()) > 0;
}

}  // namespace

std::vector<GraphTransition> ApplyOCOTransitions(ComboGroupState& group) {
  std::vector<GraphTransition> applied;
  for (const auto& edge : group.edges) {
    if (edge.type != GroupEdgeType::kOcoSibling) continue;
    const GroupLeg* source = group.FindLeg(edge.source_leg_id);
    GroupLeg* target = group.FindLeg(edge.target_leg_id);
    if (source == nullptr || target == nullptr) continue;
    if (!IsFilled(*source)) continue;
    // Идемпотентность: уже отменённую ногу повторно не трогаем.
    if (target->status == GroupLegStatus::kCancelled) continue;

    target->status = GroupLegStatus::kCancelled;
    applied.push_back(GraphTransition{
        edge.source_leg_id, edge.target_leg_id, "oco_cancel",
        group.parent_order_id + ":" + edge.source_leg_id + "->" + edge.target_leg_id +
            ":oco_cancel"});
  }
  return applied;
}

std::vector<GraphTransition> ResizeBracketExits(ComboGroupState& group) {
  std::vector<GraphTransition> applied;
  for (const auto& edge : group.edges) {
    if (edge.type != GroupEdgeType::kBracketEntry) continue;
    const GroupLeg* entry = group.FindLeg(edge.source_leg_id);
    GroupLeg* exit = group.FindLeg(edge.target_leg_id);
    if (entry == nullptr || exit == nullptr) continue;
    if (Decimal::cmp(entry->filled_cum, Decimal::zero()) <= 0) continue;
    // Идемпотентность: q_max уже равен исполненному entry — перехода нет.
    if (Decimal::cmp(exit->q_max, entry->filled_cum) == 0) continue;

    // Q_tp = Q_sl = Q_entry^filled (ADR — bracket resize).
    exit->q_max = entry->filled_cum;
    applied.push_back(GraphTransition{
        edge.source_leg_id, edge.target_leg_id, "bracket_resize",
        group.parent_order_id + ":" + edge.source_leg_id + "->" + edge.target_leg_id +
            ":bracket_resize:" + entry->filled_cum.to_string()});
  }
  return applied;
}

std::vector<GraphTransition> ApplyConditionalActivations(ComboGroupState& group,
                                                         const ReferencePrices& prices) {
  std::vector<GraphTransition> applied;
  for (const auto& edge : group.edges) {
    if (edge.type != GroupEdgeType::kConditional) continue;
    GroupLeg* target = group.FindLeg(edge.target_leg_id);
    if (target == nullptr) continue;
    // Идемпотентность: активируем только из waiting_for_trigger.
    if (target->status != GroupLegStatus::kWaitingForTrigger) continue;
    if (!EvaluateTrigger(edge.condition, prices)) continue;

    target->status = GroupLegStatus::kActive;
    applied.push_back(GraphTransition{
        edge.source_leg_id, edge.target_leg_id, "conditional_activate",
        group.parent_order_id + ":" + edge.source_leg_id + "->" + edge.target_leg_id +
            ":conditional_activate"});
  }
  return applied;
}

}  // namespace cex::matching::domain
