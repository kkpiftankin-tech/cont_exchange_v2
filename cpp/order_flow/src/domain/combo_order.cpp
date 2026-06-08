// ============================================================================
// combo_order.cpp — реализация инвариантов domain-слоя F-09.
// См. combo_order.hpp. Источник правил: IN-011 §8, §15, §20; ADR-031/032.
// ============================================================================

#include "domain/combo_order.hpp"

namespace cex::order_flow::domain {

namespace {

using cex::common::Decimal;

/// x > 0 ?
bool IsPositive(const Decimal& x) {
  return Decimal::cmp(x, Decimal::zero()) > 0;
}

/// x >= 0 ?
bool IsNonNegative(const Decimal& x) {
  return Decimal::cmp(x, Decimal::zero()) >= 0;
}

/// a <= b ?
bool Lte(const Decimal& a, const Decimal& b) {
  return Decimal::cmp(a, b) <= 0;
}

}  // namespace

cex::common::Decimal Leg::remaining_qty() const {
  const auto remaining = Decimal::sub(q_max, filled_cum);
  if (Decimal::cmp(remaining, Decimal::zero()) < 0) {
    return Decimal::zero();
  }
  return remaining;
}

std::optional<DomainError> Leg::validate() const {
  if (instrument_symbol.empty()) {
    return DomainError{"LEG_INSTRUMENT_EMPTY", "leg instrument_symbol is empty"};
  }
  if (side == Side::kUnspecified) {
    return DomainError{"LEG_SIDE_UNSPECIFIED", "leg side is unspecified"};
  }
  // Ровно одно из {ratio, weight} (IN-011 §8.3).
  if (ratio.has_value() == weight.has_value()) {
    return DomainError{"LEG_RATIO_XOR_WEIGHT",
                       "exactly one of {ratio, weight} must be set"};
  }
  if (!IsPositive(p_low)) {
    return DomainError{"LEG_PRICE_RANGE", "p_low must be > 0"};
  }
  if (Decimal::cmp(p_high, p_low) < 0) {
    return DomainError{"LEG_PRICE_RANGE", "p_high must be >= p_low"};
  }
  if (!IsPositive(q_rate)) {
    return DomainError{"LEG_Q_RATE", "q_rate must be > 0"};
  }
  if (!IsPositive(q_max)) {
    return DomainError{"LEG_Q_MAX", "q_max must be > 0"};
  }
  if (!IsNonNegative(filled_cum)) {
    return DomainError{"LEG_FILLED_CUM", "filled_cum must be >= 0"};
  }
  if (!Lte(filled_cum, q_max)) {
    return DomainError{"LEG_FILLED_CUM_EXCEEDS_QMAX",
                       "filled_cum must be <= q_max"};
  }
  return std::nullopt;
}

std::optional<DomainError> MultiLegConstraint::validate() const {
  if (constraint_id.empty()) {
    return DomainError{"CONSTRAINT_ID_EMPTY", "constraint_id is empty"};
  }
  if (value.has_value() && !IsNonNegative(*value)) {
    return DomainError{"CONSTRAINT_VALUE_NEGATIVE", "constraint value must be >= 0"};
  }
  if (value_bps.has_value() && *value_bps < 0) {
    return DomainError{"CONSTRAINT_VALUE_BPS_NEGATIVE", "value_bps must be >= 0"};
  }
  switch (type) {
    case ConstraintType::kSpreadRange:
      if (!lower.has_value() || !upper.has_value()) {
        return DomainError{"CONSTRAINT_SPREAD_BOUNDS",
                           "spread_range requires lower and upper"};
      }
      if (Decimal::cmp(*lower, *upper) > 0) {
        return DomainError{"CONSTRAINT_SPREAD_BOUNDS", "lower must be <= upper"};
      }
      break;
    case ConstraintType::kMaxTotalNotional:
      if (!value.has_value() || !IsPositive(*value)) {
        return DomainError{"CONSTRAINT_NOTIONAL_VALUE",
                           "max_total_notional requires value > 0"};
      }
      break;
    default:
      break;
  }
  return std::nullopt;
}

std::optional<DomainError> ConditionalLink::validate() const {
  if (link_id.empty()) {
    return DomainError{"LINK_ID_EMPTY", "link_id is empty"};
  }
  if (from_leg_id.empty() || to_leg_id.empty()) {
    return DomainError{"LINK_LEG_EMPTY", "from_leg_id/to_leg_id must be set"};
  }
  if (from_leg_id == to_leg_id) {
    return DomainError{"LINK_SELF_LOOP", "from_leg_id must differ from to_leg_id"};
  }
  return std::nullopt;
}

std::optional<DomainError> ComboOrder::validate() const {
  if (combo_order_id.empty()) {
    return DomainError{"COMBO_ID_EMPTY", "combo_order_id is empty"};
  }
  if (execution_mode == ExecutionMode::kUnspecified) {
    return DomainError{"COMBO_MODE_UNSPECIFIED", "execution_mode is unspecified"};
  }
  if (legs.size() < 2) {
    return DomainError{"COMBO_TOO_FEW_LEGS", "combo requires at least 2 legs"};
  }
  for (const auto& leg : legs) {
    if (auto err = leg.validate()) {
      return err;
    }
  }
  for (const auto& constraint : constraints) {
    if (auto err = constraint.validate()) {
      return err;
    }
  }
  for (const auto& link : conditional_links) {
    if (auto err = link.validate()) {
      return err;
    }
  }
  // multileg_vector_solver обязан иметь atomicity_policy (AC-F09-001, IN-011 §21).
  if (execution_mode == ExecutionMode::kMultilegVectorSolver &&
      atomicity_policy == AtomicityPolicy::kUnspecified) {
    return DomainError{"COMBO_POLICY_REQUIRED",
                       "multileg_vector_solver requires atomicity_policy"};
  }
  // strict_atomic несовместима с external_compensating scope (AC-F09-006, ADR-031).
  if (atomicity_policy == AtomicityPolicy::kStrictAtomic &&
      atomicity_scope == AtomicityScope::kExternalCompensating) {
    return DomainError{"COMBO_STRICT_EXTERNAL_SCOPE",
                       "strict_atomic incompatible with external_compensating scope"};
  }
  if (min_execution_scale.has_value()) {
    const auto& s = *min_execution_scale;
    const Decimal one{1, 0};
    if (!IsNonNegative(s) || Decimal::cmp(s, one) > 0) {
      return DomainError{"COMBO_MIN_SCALE_RANGE",
                         "min_execution_scale must be in [0, 1]"};
    }
  }
  if (max_ratio_deviation_bps.has_value() && *max_ratio_deviation_bps < 0) {
    return DomainError{"COMBO_RATIO_DEV_NEGATIVE",
                       "max_ratio_deviation_bps must be >= 0"};
  }
  return std::nullopt;
}

std::optional<DomainError> BatchOrder::validate() const {
  if (batch_order_id.empty()) {
    return DomainError{"BATCH_ID_EMPTY", "batch_order_id is empty"};
  }
  if (user_id.empty() || account_id.empty()) {
    return DomainError{"BATCH_OWNER_EMPTY", "user_id/account_id must be set"};
  }
  if (child_refs.empty()) {
    return DomainError{"BATCH_NO_CHILDREN", "batch requires at least 1 child_ref"};
  }
  return std::nullopt;
}

}  // namespace cex::order_flow::domain
