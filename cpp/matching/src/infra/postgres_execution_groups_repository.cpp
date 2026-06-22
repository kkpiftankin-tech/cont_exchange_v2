// ============================================================================
// postgres_execution_groups_repository.cpp — F-09 (T-F09-047). См. .hpp.
//
// Строки статусов/политик совпадают с DDL (infra/postgres/init.sql, блок F-09).
// Деньги — NUMERIC через Decimal::to_string() (CLAUDE.md §9, без double).
// ============================================================================

#include "infra/postgres_execution_groups_repository.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

#include "cex/common/decimal.hpp"
#include "fob/orders/v1/combo.pb.h"

namespace cex::matching::infra {

namespace {

namespace mv1 = fob::matching::v1;
namespace ov1 = fob::orders::v1;
using cex::common::Decimal;

std::string GroupStatusDb(mv1::GroupStatus s) {
  switch (s) {
    case mv1::GROUP_STATUS_FILLED: return "filled";
    case mv1::GROUP_STATUS_PARTIAL: return "partial";
    case mv1::GROUP_STATUS_WAITING_NEXT_BATCH: return "waiting_next_batch";
    case mv1::GROUP_STATUS_CANCELLED_BY_ATOMICITY: return "cancelled_by_atomicity";
    case mv1::GROUP_STATUS_DEGRADED: return "degraded";
    case mv1::GROUP_STATUS_COMPENSATING: return "compensating";
    case mv1::GROUP_STATUS_ROLLBACK_PENDING: return "rollback_pending";
    case mv1::GROUP_STATUS_ROLLEDBACK: return "rolledback";
    case mv1::GROUP_STATUS_FAILED: return "failed";
    default: return "waiting_next_batch";
  }
}

std::string ExecModeDb(ov1::ExecutionMode m) {
  return m == ov1::EXECUTION_MODE_ORCHESTRATION_ONLY ? "orchestration_only"
                                                     : "multileg_vector_solver";
}

std::string PolicyDb(ov1::AtomicityPolicy p) {
  switch (p) {
    case ov1::ATOMICITY_POLICY_STRICT_ATOMIC: return "strict_atomic";
    case ov1::ATOMICITY_POLICY_SCALABLE_ATOMIC: return "scalable_atomic";
    case ov1::ATOMICITY_POLICY_BEST_EFFORT: return "best_effort";
    case ov1::ATOMICITY_POLICY_SEQUENTIAL_FALLBACK: return "sequential_fallback";
    case ov1::ATOMICITY_POLICY_EXTERNAL_COMPENSATING: return "external_compensating";
    default: return "best_effort";
  }
}

std::string ScopeDb(ov1::AtomicityScope s) {
  switch (s) {
    case ov1::ATOMICITY_SCOPE_INTERNAL_BATCH: return "internal_batch";
    case ov1::ATOMICITY_SCOPE_VENUE_NATIVE: return "venue_native";
    case ov1::ATOMICITY_SCOPE_EXTERNAL_COMPENSATING: return "external_compensating";
    case ov1::ATOMICITY_SCOPE_NONE: return "none";
    default: return "internal_batch";
  }
}


std::string LegResultsJson(const mv1::ExecutionGroup& eg) {
  std::ostringstream os;
  os << '[';
  bool first = true;
  for (const auto& lr : eg.leg_results()) {
    if (!first) os << ',';
    first = false;
    os << "{\"legId\":\"" << lr.leg_id() << "\",\"execQty\":\""
       << Decimal::from_proto(lr.exec_qty()).to_string() << "\",\"execPrice\":\""
       << Decimal::from_proto(lr.exec_price()).to_string() << "\",\"fillId\":\""
       << lr.fill_id() << "\"}";
  }
  os << ']';
  return os.str();
}

std::string StringArrayJson(const google::protobuf::RepeatedPtrField<std::string>& items) {
  std::ostringstream os;
  os << '[';
  bool first = true;
  for (const auto& s : items) {
    if (!first) os << ',';
    first = false;
    os << '"' << s << '"';
  }
  os << ']';
  return os.str();
}

std::string DiagnosticsJson(const mv1::ExecutionGroup& eg) {
  std::ostringstream os;
  os << "{\"bindingLegs\":"
     << StringArrayJson(eg.solver_diagnostics().binding_leg_ids()) << '}';
  return os.str();
}

}  // namespace

PostgresExecutionGroupsRepository::PostgresExecutionGroupsRepository(std::string dsn)
    : connection_factory_([dsn = std::move(dsn)]() {
        return std::make_unique<pqxx::connection>(dsn);
      }) {}

PostgresExecutionGroupsRepository::PostgresExecutionGroupsRepository(ConnectionFactory factory)
    : connection_factory_(std::move(factory)) {}

void PostgresExecutionGroupsRepository::PersistExecutionGroup(
    const fob::matching::v1::ExecutionGroup& eg) {
  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }
  pqxx::work tx(*conn);

  // 1) execution_groups (идемпотентно по PK). affected_rows() == 0 → дубликат
  //    at-least-once доставки: НЕ применяем fills повторно (иначе double-count).
  const pqxx::result inserted = tx.exec_params(
      R"SQL(
INSERT INTO execution_groups
  (execution_group_id, batch_id, parent_order_id, execution_mode, group_status,
   execution_scale, atomicity_policy, atomicity_scope, fallback_action,
   ratio_deviation_bps, leg_results, violated_constraints, solver_diagnostics)
VALUES ($1::uuid, $2, $3::uuid, $4, $5, $6::numeric, $7, $8,
        NULLIF($9,'')::text, $10::int, $11::jsonb, $12::jsonb, $13::jsonb)
ON CONFLICT (execution_group_id) DO NOTHING
)SQL",
      eg.execution_group_id(), eg.batch_id(), eg.parent_order_id(),
      ExecModeDb(eg.execution_mode()), GroupStatusDb(eg.group_status()),
      Decimal::from_proto(eg.execution_scale()).to_string(),
      PolicyDb(eg.atomicity_policy()), ScopeDb(eg.atomicity_scope()),
      eg.fallback_action(), static_cast<int>(eg.ratio_deviation_bps()),
      LegResultsJson(eg), StringArrayJson(eg.violated_constraints()),
      DiagnosticsJson(eg));

  if (inserted.affected_rows() == 0) {
    tx.commit();  // дубликат группы → идемпотентно, без побочных эффектов
    return;
  }

  // 2) group_state_transitions (идемпотентно по idempotency_key = execution_group_id).
  tx.exec_params(
      R"SQL(
INSERT INTO group_state_transitions (group_id, to_status, batch_id, reason, idempotency_key)
VALUES ($1::uuid, $2, $3, $4, $5)
ON CONFLICT (idempotency_key) DO NOTHING
)SQL",
      eg.execution_group_id(), GroupStatusDb(eg.group_status()), eg.batch_id(),
      "grouped_solve", eg.execution_group_id());

  // 3) Применяем fills к ногам и пересчитываем статус combo — только если группа
  //    реально исполнилась (есть leg_results). Накопление filled_cum между batch-
  //    циклами устраняет переисполнение partial-групп (Known Gap MVP-2).
  if (eg.leg_results_size() > 0) {
    for (const auto& lr : eg.leg_results()) {
      tx.exec_params(
          "UPDATE combo_order_legs "
          "SET filled_cum = LEAST(filled_cum + $3::numeric, q_max) "
          "WHERE parent_order_id = $1::uuid AND leg_id = $2::uuid",
          eg.parent_order_id(), lr.leg_id(),
          Decimal::from_proto(lr.exec_qty()).to_string());
    }
    // combo → filled, когда ЛЮБАЯ нога достигла q_max. Для ratio-locked группы
    // (scalable/strict) исчерпание binding-ноги = максимум группы: остальные
    // ноги уже на своей ratio-доле и больше исполнить нельзя (иначе ломается
    // соотношение). Без этого группа застревала бы в partially_filled и
    // переисполнялась каждый batch (G_feasible=0). Иначе — partially_filled.
    tx.exec_params(
        R"SQL(
UPDATE combo_orders SET status = CASE
    WHEN EXISTS (SELECT 1 FROM combo_order_legs
                 WHERE parent_order_id = $1::uuid AND filled_cum >= q_max)
      THEN 'filled' ELSE 'partially_filled' END,
    updated_at = NOW()
WHERE combo_order_id = $1::uuid AND status NOT IN ('filled','cancelled')
)SQL",
        eg.parent_order_id());

    // Когда combo достигла 'filled', закрываем ВСЕ её ещё активные ноги:
    // ratio-locked группа больше не исполняется (binding-нога исчерпана), и без
    // этого не-binding ноги застревали бы в 'active' навсегда — видны как
    // «зависшие» в Профиле. Идемпотентно (повторный батч уже DO NOTHING выше).
    tx.exec_params(
        R"SQL(
UPDATE combo_order_legs SET status = 'filled'
WHERE parent_order_id = $1::uuid
  AND status = 'active'
  AND EXISTS (SELECT 1 FROM combo_orders
              WHERE combo_order_id = $1::uuid AND status = 'filled')
)SQL",
        eg.parent_order_id());
  }

  tx.commit();
}

}  // namespace cex::matching::infra
