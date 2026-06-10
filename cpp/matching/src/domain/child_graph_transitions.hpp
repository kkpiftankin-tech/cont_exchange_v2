#pragma once
// ============================================================================
// child_graph_transitions.hpp — F-09 (T-F09-045). Matching domain (pure).
//
// Переходы графа дочерних связей combo-группы:
//   * OCO: исполнение ноги-источника отменяет ноги-сиблинги (target).
//   * Bracket: исполнение entry-ноги ресайзит exit-ноги (TP/SL) до filled entry.
// Обе функции ИДЕМПОТЕНТНЫ: повторный вызов на уже-применённом состоянии не
// порождает новых переходов и не меняет состояние (AC-F09-007/008). Каждый
// переход несёт idempotency_key для записи в group_state_transitions.
// ============================================================================

#include <string>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/trigger_condition.hpp"  // TriggerCondition + ReferencePrices

namespace cex::matching::domain {

enum class GroupLegStatus {
  kActive,
  kWaitingForTrigger,
  kPartiallyFilled,
  kFilled,
  kCancelled,
};

enum class GroupEdgeType {
  kOcoSibling,   ///< source fill → cancel target
  kBracketEntry, ///< entry fill → resize target (TP/SL)
  kConditional,  ///< прочее (MVP-4)
};

struct GroupLeg {
  std::string leg_id;
  GroupLegStatus status{GroupLegStatus::kActive};
  cex::common::Decimal q_max{};
  cex::common::Decimal filled_cum{};
  bool is_exit{false};  ///< TP/SL exit-нога (bracket)
};

struct GroupEdge {
  GroupEdgeType type{GroupEdgeType::kConditional};
  std::string source_leg_id;
  std::string target_leg_id;
  TriggerCondition condition;  ///< для kConditional (present=false → безусловно)
};

struct ComboGroupState {
  std::string parent_order_id;
  std::vector<GroupLeg> legs;
  std::vector<GroupEdge> edges;

  [[nodiscard]] GroupLeg* FindLeg(const std::string& leg_id) {
    for (auto& l : legs) if (l.leg_id == leg_id) return &l;
    return nullptr;
  }
};

/// Применённый переход графа (для идемпотентной записи в group_state_transitions).
struct GraphTransition {
  std::string source_leg_id;
  std::string target_leg_id;
  std::string action;          ///< "oco_cancel" | "bracket_resize"
  std::string idempotency_key; ///< parent:source->target:action:value
};

/// OCO: для каждого ребра kOcoSibling, если source-нога исполнена (filled_cum>0
/// или статус Filled), переводит target-ногу в Cancelled. Идемпотентно: уже
/// отменённые target не порождают новый переход. Возвращает применённые переходы.
[[nodiscard]] std::vector<GraphTransition> ApplyOCOTransitions(ComboGroupState& group);

/// Bracket: для каждого ребра kBracketEntry, если entry-нога (source) имеет
/// filled_cum>0, ресайзит target exit-ногу: q_max := entry.filled_cum.
/// Идемпотентно: если q_max уже равен filled entry — перехода нет.
[[nodiscard]] std::vector<GraphTransition> ResizeBracketExits(ComboGroupState& group);

/// Conditional (MVP-4.1): для каждого ребра kConditional, если target-нога в
/// waiting_for_trigger и условие выполнено (EvaluateTrigger по рыночным ценам),
/// активирует target (→ Active). Идемпотентно: уже активную ногу не трогаем.
[[nodiscard]] std::vector<GraphTransition> ApplyConditionalActivations(
    ComboGroupState& group, const ReferencePrices& prices);

}  // namespace cex::matching::domain
