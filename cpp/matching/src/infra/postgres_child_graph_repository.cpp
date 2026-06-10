// ============================================================================
// postgres_child_graph_repository.cpp — F-09 (MVP-4, ADR-035). См. .hpp.
// ============================================================================

#include "infra/postgres_child_graph_repository.hpp"

#include <unordered_set>
#include <utility>

#include "infra/postgres/decimal_conversion.hpp"  // ParsePgNumeric / ToPgNumeric

namespace cex::matching::infra {

namespace {

using cex::common::Decimal;
namespace d = cex::matching::domain;
using cex::matching::infra::postgres::ParsePgNumeric;
using cex::matching::infra::postgres::ToPgNumeric;

d::GroupLegStatus LegStatusFromDb(const std::string& s) {
  if (s == "waiting_for_trigger") return d::GroupLegStatus::kWaitingForTrigger;
  if (s == "partially_filled") return d::GroupLegStatus::kPartiallyFilled;
  if (s == "filled") return d::GroupLegStatus::kFilled;
  if (s == "cancelled") return d::GroupLegStatus::kCancelled;
  return d::GroupLegStatus::kActive;  // active/inactive/blocked/… → active для графа
}

d::GroupEdgeType EdgeTypeFromDb(const std::string& s) {
  if (s == "oco") return d::GroupEdgeType::kOcoSibling;
  if (s == "bracket") return d::GroupEdgeType::kBracketEntry;
  return d::GroupEdgeType::kConditional;
}

Decimal NumOrZero(const pqxx::field& f) {
  return f.is_null() ? Decimal::zero() : ParsePgNumeric(f.as<std::string>());
}

}  // namespace

PostgresChildGraphRepository::PostgresChildGraphRepository(std::string dsn)
    : connection_factory_([dsn = std::move(dsn)]() {
        return std::make_unique<pqxx::connection>(dsn);
      }) {}

PostgresChildGraphRepository::PostgresChildGraphRepository(ConnectionFactory factory)
    : connection_factory_(std::move(factory)) {}

d::ComboGroupState PostgresChildGraphRepository::LoadComboGroupState(
    const std::string& parent_order_id) {
  d::ComboGroupState state;
  state.parent_order_id = parent_order_id;
  if (parent_order_id.empty()) return state;

  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  const pqxx::result legs = tx.exec_params(R"SQL(
SELECT leg_id::text, status, q_max, filled_cum
FROM combo_order_legs WHERE parent_order_id = $1::uuid ORDER BY leg_id
)SQL",
                                           parent_order_id);
  const pqxx::result links = tx.exec_params(R"SQL(
SELECT link_type, from_leg_id::text, to_leg_id::text
FROM conditional_links WHERE parent_order_id = $1::uuid ORDER BY link_id
)SQL",
                                            parent_order_id);
  tx.commit();

  // exit-ноги (bracket) = target bracket-рёбер.
  std::unordered_set<std::string> exit_legs;
  for (const auto& e : links) {
    if (e["link_type"].as<std::string>() == "bracket") {
      exit_legs.insert(e["to_leg_id"].as<std::string>());
    }
  }

  for (const auto& l : legs) {
    d::GroupLeg leg;
    leg.leg_id = l["leg_id"].as<std::string>();
    leg.status = LegStatusFromDb(l["status"].as<std::string>());
    leg.q_max = NumOrZero(l["q_max"]);
    leg.filled_cum = NumOrZero(l["filled_cum"]);
    leg.is_exit = exit_legs.count(leg.leg_id) > 0;
    state.legs.push_back(std::move(leg));
  }
  for (const auto& e : links) {
    d::GroupEdge edge;
    edge.type = EdgeTypeFromDb(e["link_type"].as<std::string>());
    edge.source_leg_id = e["from_leg_id"].as<std::string>();
    edge.target_leg_id = e["to_leg_id"].as<std::string>();
    state.edges.push_back(std::move(edge));
  }
  return state;
}

void PostgresChildGraphRepository::PersistChildGraphTransitions(
    const std::string& execution_group_id, const std::string& batch_id,
    const d::ComboGroupState& group, const std::vector<d::GraphTransition>& transitions) {
  if (transitions.empty()) return;

  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  for (const auto& t : transitions) {
    if (t.action == "oco_cancel") {
      tx.exec_params(
          "UPDATE combo_order_legs SET status = 'cancelled' "
          "WHERE leg_id = $1::uuid AND status <> 'cancelled'",
          t.target_leg_id);
    } else if (t.action == "bracket_resize") {
      // Новый q_max exit-ноги + активация (из состояния после перехода).
      const d::GroupLeg* leg = nullptr;
      for (const auto& l : group.legs) {
        if (l.leg_id == t.target_leg_id) { leg = &l; break; }
      }
      if (leg != nullptr) {
        tx.exec_params(
            "UPDATE combo_order_legs SET q_max = $2::numeric, status = 'active' "
            "WHERE leg_id = $1::uuid",
            t.target_leg_id, ToPgNumeric(leg->q_max));
      }
    }
    // group_id = execution_group_id триггерящего batch (ADR-035 §1); идемпотентно.
    tx.exec_params(R"SQL(
INSERT INTO group_state_transitions
  (group_id, from_status, to_status, batch_id, reason, idempotency_key)
VALUES ($1::uuid, 'active', $2, $3, $4, $5)
ON CONFLICT (idempotency_key) DO NOTHING
)SQL",
                   execution_group_id, t.action, batch_id, t.action, t.idempotency_key);
  }
  tx.commit();
}

}  // namespace cex::matching::infra
