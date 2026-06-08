// ============================================================================
// postgres_combo_order_repository.cpp — F-09 (T-F09-034). См. .hpp.
//
// Все enum→строка мапперы совпадают с CHECK-ограничениями DDL
// (infra/postgres/init.sql, блок F-09). Деньги — NUMERIC через
// cex::common::Decimal::to_string() (CLAUDE.md §9, никаких double).
//
// NOTE (идемпотентность): combo_orders.atomicity_policy / atomicity_scope /
// fallback_policy — NOT NULL в DDL. Для orchestration_only, где domain не
// требует политику, при записи подставляются честные дефолты:
// best_effort / none / scale_down. Маппинг client_combo_id → combo_order_id —
// в CreateComboOrderUseCase (T-F09-031); здесь идемпотентность по PK.
// ============================================================================

#include "infra/postgres_combo_order_repository.hpp"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cex::order_flow::infra {

namespace {

namespace d = cex::order_flow::domain;
using cex::common::Decimal;

std::string ExecModeDb(d::ExecutionMode m) {
  switch (m) {
    case d::ExecutionMode::kOrchestrationOnly:    return "orchestration_only";
    case d::ExecutionMode::kMultilegVectorSolver: return "multileg_vector_solver";
    default: throw std::invalid_argument("combo: execution_mode unspecified");
  }
}

std::string ComboTypeDb(d::ComboType t) {
  switch (t) {
    case d::ComboType::kPair: return "pair";
    case d::ComboType::kBasket: return "basket";
    case d::ComboType::kSpread: return "spread";
    case d::ComboType::kConditional: return "conditional";
    case d::ComboType::kOco: return "oco";
    case d::ComboType::kBracket: return "bracket";
    case d::ComboType::kFactor: return "factor";
    case d::ComboType::kBudget: return "budget";
    default: throw std::invalid_argument("combo: combo_type unspecified");
  }
}

// batch_orders.order_type CHECK не включает factor/budget → clamp к 'combo'.
std::string BatchTypeDb(d::ComboType t) {
  switch (t) {
    case d::ComboType::kPair: case d::ComboType::kBasket: case d::ComboType::kSpread:
    case d::ComboType::kConditional: case d::ComboType::kOco: case d::ComboType::kBracket:
      return ComboTypeDb(t);
    case d::ComboType::kFactor: case d::ComboType::kBudget:
      return "combo";
    default: return "batch";
  }
}

std::string AtomicityPolicyDb(d::AtomicityPolicy p) {
  switch (p) {
    case d::AtomicityPolicy::kStrictAtomic: return "strict_atomic";
    case d::AtomicityPolicy::kScalableAtomic: return "scalable_atomic";
    case d::AtomicityPolicy::kBestEffort: return "best_effort";
    case d::AtomicityPolicy::kSequentialFallback: return "sequential_fallback";
    case d::AtomicityPolicy::kExternalCompensating: return "external_compensating";
    default: return "best_effort";  // orchestration_only honest default
  }
}

std::string AtomicityScopeDb(d::AtomicityScope s) {
  switch (s) {
    case d::AtomicityScope::kInternalBatch: return "internal_batch";
    case d::AtomicityScope::kVenueNative: return "venue_native";
    case d::AtomicityScope::kExternalCompensating: return "external_compensating";
    default: return "none";
  }
}

std::string FallbackPolicyDb(d::FallbackPolicy f) {
  switch (f) {
    case d::FallbackPolicy::kScaleDown: return "scale_down";
    case d::FallbackPolicy::kWaitNextBatch: return "wait_next_batch";
    case d::FallbackPolicy::kCancel: return "cancel";
    case d::FallbackPolicy::kDegrade: return "degrade";
    case d::FallbackPolicy::kCompensate: return "compensate";
    default: return "scale_down";
  }
}

std::string RatioBasisDb(d::RatioBasis r) {
  switch (r) {
    case d::RatioBasis::kNotionalWeight: return "notional_weight";
    case d::RatioBasis::kQuantityRatio: return "quantity_ratio";
    default: return "";  // → NULL
  }
}

std::string ParentStatusDb(d::ParentOrderStatus s) {
  switch (s) {
    case d::ParentOrderStatus::kDraft: return "draft";
    case d::ParentOrderStatus::kRiskPending: return "risk_pending";
    case d::ParentOrderStatus::kActive: return "active";
    case d::ParentOrderStatus::kWaitingForTrigger: return "waiting_for_trigger";
    case d::ParentOrderStatus::kPartiallyFilled: return "partially_filled";
    case d::ParentOrderStatus::kFilled: return "filled";
    case d::ParentOrderStatus::kCancelled: return "cancelled";
    case d::ParentOrderStatus::kExpired: return "expired";
    case d::ParentOrderStatus::kDegraded: return "degraded";
    case d::ParentOrderStatus::kRollbackPending: return "rollback_pending";
    case d::ParentOrderStatus::kRolledback: return "rolledback";
    case d::ParentOrderStatus::kRejected: return "rejected";
    default: return "draft";
  }
}

d::ParentOrderStatus ParentStatusFromDb(const std::string& s) {
  if (s == "risk_pending") return d::ParentOrderStatus::kRiskPending;
  if (s == "active") return d::ParentOrderStatus::kActive;
  if (s == "waiting_for_trigger") return d::ParentOrderStatus::kWaitingForTrigger;
  if (s == "partially_filled") return d::ParentOrderStatus::kPartiallyFilled;
  if (s == "filled") return d::ParentOrderStatus::kFilled;
  if (s == "cancelled") return d::ParentOrderStatus::kCancelled;
  if (s == "expired") return d::ParentOrderStatus::kExpired;
  if (s == "degraded") return d::ParentOrderStatus::kDegraded;
  if (s == "rollback_pending") return d::ParentOrderStatus::kRollbackPending;
  if (s == "rolledback") return d::ParentOrderStatus::kRolledback;
  if (s == "rejected") return d::ParentOrderStatus::kRejected;
  return d::ParentOrderStatus::kDraft;
}

std::string LegStatusDb(d::LegStatus s) {
  switch (s) {
    case d::LegStatus::kInactive: return "inactive";
    case d::LegStatus::kActive: return "active";
    case d::LegStatus::kWaitingForTrigger: return "waiting_for_trigger";
    case d::LegStatus::kPartiallyFilled: return "partially_filled";
    case d::LegStatus::kFilled: return "filled";
    case d::LegStatus::kCancelled: return "cancelled";
    case d::LegStatus::kBlockedByGroup: return "blocked_by_group";
    case d::LegStatus::kBlockedByAtomicity: return "blocked_by_atomicity";
    case d::LegStatus::kFailedExternal: return "failed_external";
    case d::LegStatus::kCompensated: return "compensated";
    default: return "inactive";
  }
}

std::string SideDb(d::Side s) {
  switch (s) {
    case d::Side::kBuy: return "buy";
    case d::Side::kSell: return "sell";
    default: throw std::invalid_argument("combo leg: side unspecified");
  }
}

std::string ConstraintTypeDb(d::ConstraintType t) {
  switch (t) {
    case d::ConstraintType::kMaxWeightDeviation: return "max_weight_deviation";
    case d::ConstraintType::kMaxTotalNotional: return "max_total_notional";
    case d::ConstraintType::kSpreadRange: return "spread_range";
    case d::ConstraintType::kFactorNeutrality: return "factor_neutrality";
    case d::ConstraintType::kMaxLeverage: return "max_leverage";
    case d::ConstraintType::kMaxMargin: return "max_margin";
    case d::ConstraintType::kRiskLimit: return "risk_limit";
    case d::ConstraintType::kRatioEquality: return "ratio_equality";
    default: return "max_total_notional";
  }
}

std::string LinkTypeDb(d::ConditionalLinkType t) {
  switch (t) {
    case d::ConditionalLinkType::kOco: return "oco";
    case d::ConditionalLinkType::kBracket: return "bracket";
    default: return "conditional";
  }
}

std::string OptDec(const std::optional<Decimal>& v) { return v ? v->to_string() : std::string{}; }

std::string CoefficientsJson(const std::unordered_map<std::string, Decimal>& m) {
  if (m.empty()) return "{}";
  std::string s = "{";
  bool first = true;
  for (const auto& [k, v] : m) {
    if (!first) s += ",";
    first = false;
    s += "\"" + k + "\":\"" + v.to_string() + "\"";  // string value сохраняет precision
  }
  return s + "}";
}

std::string VenuePrefsArray(const std::vector<std::string>& v) {
  std::string s = "{";
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i != 0) s += ",";
    s += "\"" + v[i] + "\"";
  }
  return s + "}";
}

}  // namespace

PostgresComboOrderRepository::PostgresComboOrderRepository(std::string connection_string)
    : connection_factory_([conn_str = std::move(connection_string)]() {
        return std::make_unique<pqxx::connection>(conn_str);
      }) {}

PostgresComboOrderRepository::PostgresComboOrderRepository(ConnectionFactory connection_factory)
    : connection_factory_(std::move(connection_factory)) {
  if (!connection_factory_) {
    throw std::invalid_argument("PostgresComboOrderRepository requires a valid connection factory");
  }
}

void PostgresComboOrderRepository::InsertComboOrder(
    const domain::ComboOrder& combo,
    const std::optional<domain::BatchOrder>& batch) {
  if (auto err = combo.validate()) {
    throw std::invalid_argument("InsertComboOrder: invalid combo: " + err->message);
  }

  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }
  pqxx::work tx(*conn);

  // 1) BatchOrder (optional parent container).
  if (batch.has_value()) {
    tx.exec_params(R"SQL(
INSERT INTO batch_orders (batch_order_id, user_id, account_id, order_type, execution_mode, status)
VALUES ($1::uuid, $2, $3, $4, $5, $6)
ON CONFLICT (batch_order_id) DO NOTHING
)SQL",
                   batch->batch_order_id, batch->user_id, batch->account_id,
                   BatchTypeDb(batch->type), ExecModeDb(batch->execution_mode),
                   ParentStatusDb(batch->status));
  }

  // 2) ComboOrder (parent).
  tx.exec_params(R"SQL(
INSERT INTO combo_orders (
  combo_order_id, batch_order_id, combo_type, execution_mode, status,
  ratio_basis, atomicity_policy, atomicity_scope, fallback_policy,
  min_execution_scale, max_ratio_deviation_bps)
VALUES (
  $1::uuid, NULLIF($2,'')::uuid, $3, $4, $5,
  NULLIF($6,''), $7, $8, $9,
  NULLIF($10,'')::numeric, NULLIF($11,'')::int)
ON CONFLICT (combo_order_id) DO NOTHING
)SQL",
                 combo.combo_order_id,
                 combo.batch_order_id.value_or(""),
                 ComboTypeDb(combo.combo_type),
                 ExecModeDb(combo.execution_mode),
                 ParentStatusDb(combo.status),
                 RatioBasisDb(combo.ratio_basis),
                 AtomicityPolicyDb(combo.atomicity_policy),
                 AtomicityScopeDb(combo.atomicity_scope),
                 FallbackPolicyDb(combo.fallback_policy),
                 OptDec(combo.min_execution_scale),
                 combo.max_ratio_deviation_bps.has_value()
                     ? std::to_string(*combo.max_ratio_deviation_bps) : std::string{});

  // 3) Legs.
  for (const auto& leg : combo.legs) {
    tx.exec_params(R"SQL(
INSERT INTO combo_order_legs (
  leg_id, parent_order_id, instrument_symbol, side, ratio, weight, ratio_basis,
  p_low, p_high, q_rate, q_max, filled_cum, venue_preferences, status)
VALUES (
  $1::uuid, $2::uuid, $3, $4, NULLIF($5,'')::numeric, NULLIF($6,'')::numeric, NULLIF($7,''),
  $8::numeric, $9::numeric, $10::numeric, $11::numeric, $12::numeric, $13::text[], $14)
ON CONFLICT (leg_id) DO NOTHING
)SQL",
                   leg.leg_id, combo.combo_order_id, leg.instrument_symbol, SideDb(leg.side),
                   OptDec(leg.ratio), OptDec(leg.weight), RatioBasisDb(leg.ratio_basis),
                   leg.p_low.to_string(), leg.p_high.to_string(), leg.q_rate.to_string(),
                   leg.q_max.to_string(), leg.filled_cum.to_string(),
                   VenuePrefsArray(leg.venue_preferences), LegStatusDb(leg.status));
  }

  // 4) Constraints.
  for (const auto& c : combo.constraints) {
    tx.exec_params(R"SQL(
INSERT INTO combo_constraints (
  constraint_id, parent_order_id, constraint_type, coefficients,
  lower_bound, upper_bound, value, value_bps, severity)
VALUES (
  $1::uuid, $2::uuid, $3, $4::jsonb,
  NULLIF($5,'')::numeric, NULLIF($6,'')::numeric, NULLIF($7,'')::numeric,
  NULLIF($8,'')::int, $9)
ON CONFLICT (constraint_id) DO NOTHING
)SQL",
                   c.constraint_id, combo.combo_order_id, ConstraintTypeDb(c.type),
                   CoefficientsJson(c.coefficients), OptDec(c.lower), OptDec(c.upper),
                   OptDec(c.value),
                   c.value_bps.has_value() ? std::to_string(*c.value_bps) : std::string{},
                   c.severity == d::ConstraintSeverity::kSoft ? "soft" : "hard");
  }

  // 5) Conditional links (OCO/bracket/conditional graph).
  for (const auto& link : combo.conditional_links) {
    tx.exec_params(R"SQL(
INSERT INTO conditional_links (link_id, parent_order_id, from_leg_id, to_leg_id, link_type, condition)
VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5, NULLIF($6,'')::jsonb)
ON CONFLICT (link_id) DO NOTHING
)SQL",
                   link.link_id, combo.combo_order_id, link.from_leg_id, link.to_leg_id,
                   LinkTypeDb(link.type), link.condition_json);
  }

  tx.commit();
}

void PostgresComboOrderRepository::UpdateComboStatus(const std::string& combo_order_id,
                                                     domain::ParentOrderStatus status) {
  if (combo_order_id.empty()) {
    throw std::invalid_argument("UpdateComboStatus: combo_order_id is empty");
  }
  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }
  pqxx::work tx(*conn);
  tx.exec_params(
      "UPDATE combo_orders SET status = $2, updated_at = NOW() WHERE combo_order_id = $1::uuid",
      combo_order_id, ParentStatusDb(status));
  tx.commit();
}

std::optional<domain::ParentOrderStatus> PostgresComboOrderRepository::GetComboStatus(
    const std::string& combo_order_id) {
  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }
  pqxx::work tx(*conn);
  const pqxx::result rows = tx.exec_params(
      "SELECT status FROM combo_orders WHERE combo_order_id = $1::uuid", combo_order_id);
  tx.commit();
  if (rows.empty()) {
    return std::nullopt;
  }
  return ParentStatusFromDb(rows[0][0].as<std::string>());
}

std::vector<std::string> PostgresComboOrderRepository::GetActiveLegIds(
    const std::string& combo_order_id) {
  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }
  pqxx::work tx(*conn);
  const pqxx::result rows = tx.exec_params(R"SQL(
SELECT leg_id::text FROM combo_order_legs
WHERE parent_order_id = $1::uuid AND status NOT IN ('cancelled','filled')
)SQL",
                                           combo_order_id);
  tx.commit();
  std::vector<std::string> leg_ids;
  leg_ids.reserve(rows.size());
  for (const auto& row : rows) {
    leg_ids.push_back(row[0].as<std::string>());
  }
  return leg_ids;
}

void PostgresComboOrderRepository::CancelComboAndLegs(const std::string& combo_order_id) {
  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }
  pqxx::work tx(*conn);
  tx.exec_params(
      "UPDATE combo_orders SET status = 'cancelled', updated_at = NOW() "
      "WHERE combo_order_id = $1::uuid AND status NOT IN ('filled','cancelled')",
      combo_order_id);
  // combo_order_legs не имеет колонки updated_at (в отличие от combo_orders).
  tx.exec_params(
      "UPDATE combo_order_legs SET status = 'cancelled' "
      "WHERE parent_order_id = $1::uuid AND status NOT IN ('filled','cancelled')",
      combo_order_id);
  tx.commit();
}

}  // namespace cex::order_flow::infra
