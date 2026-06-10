#pragma once
// ============================================================================
// combo_order.hpp — domain-слой F-09 (Batch / Combo / Multi-leg orders).
//
// Чистые value objects (никакого proto на domain-уровне — proto только на
// transport, ADR-032 §границы). Инварианты по IN-011 §8, §15, §20 и
// acceptance-criteria AC-F09-001..004.
//
// Сущности: BatchOrder → ComboOrder → Leg, плюс MultiLegConstraint и
// ConditionalLink (граф OCO/bracket/conditional). Деньги/qty/price —
// cex::common::Decimal (fixed-point, не double; CLAUDE.md §9).
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cex/common/decimal.hpp"

namespace cex::order_flow::domain {

// --- Режимы и политики (зеркало proto enums combo.proto, ADR-031) ----------

enum class ExecutionMode {
  kUnspecified,
  kOrchestrationOnly,      // независимые ноги, без multi-leg гарантий
  kMultilegVectorSolver,   // согласованный вектор e_g
};

enum class AtomicityPolicy {
  kUnspecified,
  kStrictAtomic,
  kScalableAtomic,
  kBestEffort,
  kSequentialFallback,
  kExternalCompensating,
};

enum class AtomicityScope {
  kUnspecified,
  kInternalBatch,
  kVenueNative,
  kExternalCompensating,
  kNone,
};

enum class ComboType {
  kUnspecified,
  kPair,
  kBasket,
  kSpread,
  kConditional,
  kOco,
  kBracket,
  kFactor,
  kBudget,
};

enum class RatioBasis {
  kUnspecified,
  kNotionalWeight,
  kQuantityRatio,
};

enum class FallbackPolicy {
  kUnspecified,
  kScaleDown,
  kWaitNextBatch,
  kCancel,
  kDegrade,
  kCompensate,
};

enum class Side {
  kUnspecified,
  kBuy,
  kSell,
};

enum class ParentOrderStatus {
  kDraft,
  kRiskPending,
  kActive,
  kWaitingForTrigger,
  kPartiallyFilled,
  kFilled,
  kCancelled,
  kExpired,
  kDegraded,
  kRollbackPending,
  kRolledback,
  kRejected,
};

enum class LegStatus {
  kInactive,
  kActive,
  kWaitingForTrigger,
  kPartiallyFilled,
  kFilled,
  kCancelled,
  kBlockedByGroup,
  kBlockedByAtomicity,
  kFailedExternal,
  kCompensated,
};

enum class ConstraintType {
  kMaxWeightDeviation,
  kMaxTotalNotional,
  kSpreadRange,
  kFactorNeutrality,
  kMaxLeverage,
  kMaxMargin,
  kRiskLimit,
  kRatioEquality,
};

enum class ConstraintSeverity {
  kHard,
  kSoft,
};

enum class ConditionalLinkType {
  kOco,
  kBracket,
  kConditional,
};

// --- Ошибка валидации domain --------------------------------------------------

struct DomainError {
  std::string code;     ///< Стабильный машинный код (напр. "LEG_PRICE_RANGE").
  std::string message;  ///< Человекочитаемое описание.
};

// --- Leg (нога) -------------------------------------------------------------

struct Leg {
  std::string leg_id;
  std::string parent_order_id;
  std::string instrument_symbol;
  Side side{Side::kUnspecified};
  std::optional<cex::common::Decimal> ratio;   ///< одно из {ratio, weight}
  std::optional<cex::common::Decimal> weight;
  RatioBasis ratio_basis{RatioBasis::kUnspecified};
  cex::common::Decimal p_low{};
  cex::common::Decimal p_high{};
  cex::common::Decimal q_rate{};
  cex::common::Decimal q_max{};
  cex::common::Decimal filled_cum{};
  std::vector<std::string> venue_preferences;
  LegStatus status{LegStatus::kInactive};

  /// remaining = max(q_max - filled_cum, 0) (IN-011 §20 F09-INV-1).
  [[nodiscard]] cex::common::Decimal remaining_qty() const;

  /// Инварианты ноги (IN-011 §8.3): p_low>0, p_high>=p_low, q_rate>0,
  /// q_max>0, 0<=filled_cum<=q_max, ровно одно из {ratio, weight}.
  [[nodiscard]] std::optional<DomainError> validate() const;
};

// --- MultiLegConstraint -----------------------------------------------------

struct MultiLegConstraint {
  std::string constraint_id;
  std::string parent_order_id;
  ConstraintType type{ConstraintType::kMaxTotalNotional};
  std::unordered_map<std::string, cex::common::Decimal> coefficients;  ///< spread/factor
  std::optional<cex::common::Decimal> lower;
  std::optional<cex::common::Decimal> upper;
  std::optional<cex::common::Decimal> value;
  std::optional<std::int32_t> value_bps;
  ConstraintSeverity severity{ConstraintSeverity::kHard};

  [[nodiscard]] std::optional<DomainError> validate() const;
};

// --- ConditionalLink (ребро графа OCO/bracket/conditional) -------------------

struct ConditionalLink {
  std::string link_id;
  std::string parent_order_id;
  std::string from_leg_id;
  std::string to_leg_id;
  ConditionalLinkType type{ConditionalLinkType::kConditional};
  std::string condition_json;  ///< NULL/"" для безусловной OCO-отмены

  [[nodiscard]] std::optional<DomainError> validate() const;
};

// --- ComboOrder -------------------------------------------------------------

struct ComboOrder {
  std::string combo_order_id;
  std::string user_id;     ///< владелец (T-F09-062): персистится → ledger postings
  std::string account_id;
  std::optional<std::string> batch_order_id;
  ComboType combo_type{ComboType::kUnspecified};
  ExecutionMode execution_mode{ExecutionMode::kUnspecified};
  AtomicityPolicy atomicity_policy{AtomicityPolicy::kUnspecified};
  AtomicityScope atomicity_scope{AtomicityScope::kUnspecified};
  FallbackPolicy fallback_policy{FallbackPolicy::kUnspecified};
  RatioBasis ratio_basis{RatioBasis::kUnspecified};
  std::optional<cex::common::Decimal> min_execution_scale;
  std::optional<std::int32_t> max_ratio_deviation_bps;
  std::vector<Leg> legs;
  std::vector<MultiLegConstraint> constraints;
  std::vector<ConditionalLink> conditional_links;
  ParentOrderStatus status{ParentOrderStatus::kDraft};

  /// Групповые инварианты (IN-011 §8.2, §3, §21, ADR-031):
  ///  - legs.size() >= 2;
  ///  - каждая нога/ограничение/ссылка валидна;
  ///  - execution_mode != kUnspecified;
  ///  - kMultilegVectorSolver требует atomicity_policy != kUnspecified;
  ///  - strict_atomic несовместима с atomicity_scope = external_compensating;
  ///  - min_execution_scale ∈ [0,1]; max_ratio_deviation_bps >= 0.
  [[nodiscard]] std::optional<DomainError> validate() const;
};

// --- BatchOrder (parent-контейнер) ------------------------------------------

struct ChildRef {
  std::string child_type;  ///< "combo" | "flow" | "conditional"
  std::string child_ref;
};

struct BatchOrder {
  std::string batch_order_id;
  std::string user_id;
  std::string account_id;
  ComboType type{ComboType::kUnspecified};
  ExecutionMode execution_mode{ExecutionMode::kUnspecified};
  ParentOrderStatus status{ParentOrderStatus::kDraft};
  std::vector<ChildRef> child_refs;

  /// len(child_refs) >= 1 (IN-011 §5 BR-F09-001).
  [[nodiscard]] std::optional<DomainError> validate() const;
};

}  // namespace cex::order_flow::domain
