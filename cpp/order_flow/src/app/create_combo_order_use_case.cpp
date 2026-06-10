// ============================================================================
// create_combo_order_use_case.cpp — F-09 (T-F09-031). См. .hpp.
// ============================================================================

#include "app/create_combo_order_use_case.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include "cex/common/log.hpp"
#include "cex/common/uuid.hpp"
#include "fob/common/v1/common.pb.h"

namespace cex::order_flow::app {

namespace {

namespace d = cex::order_flow::domain;
namespace pv1 = fob::orders::v1;
using cex::common::Decimal;

d::ExecutionMode MapExecMode(pv1::ExecutionMode m) {
  switch (m) {
    case pv1::EXECUTION_MODE_ORCHESTRATION_ONLY:     return d::ExecutionMode::kOrchestrationOnly;
    case pv1::EXECUTION_MODE_MULTILEG_VECTOR_SOLVER:  return d::ExecutionMode::kMultilegVectorSolver;
    default:                                          return d::ExecutionMode::kUnspecified;
  }
}

d::AtomicityPolicy MapPolicy(pv1::AtomicityPolicy p) {
  switch (p) {
    case pv1::ATOMICITY_POLICY_STRICT_ATOMIC:         return d::AtomicityPolicy::kStrictAtomic;
    case pv1::ATOMICITY_POLICY_SCALABLE_ATOMIC:       return d::AtomicityPolicy::kScalableAtomic;
    case pv1::ATOMICITY_POLICY_BEST_EFFORT:           return d::AtomicityPolicy::kBestEffort;
    case pv1::ATOMICITY_POLICY_SEQUENTIAL_FALLBACK:   return d::AtomicityPolicy::kSequentialFallback;
    case pv1::ATOMICITY_POLICY_EXTERNAL_COMPENSATING: return d::AtomicityPolicy::kExternalCompensating;
    default:                                          return d::AtomicityPolicy::kUnspecified;
  }
}

d::AtomicityScope MapScope(pv1::AtomicityScope s) {
  switch (s) {
    case pv1::ATOMICITY_SCOPE_INTERNAL_BATCH:        return d::AtomicityScope::kInternalBatch;
    case pv1::ATOMICITY_SCOPE_VENUE_NATIVE:          return d::AtomicityScope::kVenueNative;
    case pv1::ATOMICITY_SCOPE_EXTERNAL_COMPENSATING: return d::AtomicityScope::kExternalCompensating;
    case pv1::ATOMICITY_SCOPE_NONE:                  return d::AtomicityScope::kNone;
    default:                                         return d::AtomicityScope::kUnspecified;
  }
}

d::ComboType MapComboType(pv1::ComboType t) {
  switch (t) {
    case pv1::COMBO_TYPE_PAIR: return d::ComboType::kPair;
    case pv1::COMBO_TYPE_BASKET: return d::ComboType::kBasket;
    case pv1::COMBO_TYPE_SPREAD: return d::ComboType::kSpread;
    case pv1::COMBO_TYPE_CONDITIONAL: return d::ComboType::kConditional;
    case pv1::COMBO_TYPE_OCO: return d::ComboType::kOco;
    case pv1::COMBO_TYPE_BRACKET: return d::ComboType::kBracket;
    default: return d::ComboType::kUnspecified;
  }
}

d::RatioBasis MapRatioBasis(pv1::RatioBasis r) {
  switch (r) {
    case pv1::RATIO_BASIS_QUANTITY:        return d::RatioBasis::kQuantityRatio;
    case pv1::RATIO_BASIS_NOTIONAL_WEIGHT: return d::RatioBasis::kNotionalWeight;
    default:                               return d::RatioBasis::kUnspecified;
  }
}

d::Side MapSide(fob::common::v1::Side s) {
  switch (s) {
    case fob::common::v1::SIDE_BUY:  return d::Side::kBuy;
    case fob::common::v1::SIDE_SELL: return d::Side::kSell;
    default:                         return d::Side::kUnspecified;
  }
}

d::FallbackPolicy MapFallback(const std::string& f) {
  if (f == "scale_down") return d::FallbackPolicy::kScaleDown;
  if (f == "skip_batch") return d::FallbackPolicy::kWaitNextBatch;
  if (f == "cancel_group" || f == "cancel") return d::FallbackPolicy::kCancel;
  if (f == "best_effort") return d::FallbackPolicy::kDegrade;
  if (f == "compensate") return d::FallbackPolicy::kCompensate;
  return d::FallbackPolicy::kScaleDown;
}

d::ConstraintType MapConstraintType(const std::string& t) {
  if (t == "max_weight_deviation") return d::ConstraintType::kMaxWeightDeviation;
  if (t == "spread_range") return d::ConstraintType::kSpreadRange;
  if (t == "factor_neutrality") return d::ConstraintType::kFactorNeutrality;
  if (t == "ratio_eq" || t == "ratio_equality") return d::ConstraintType::kRatioEquality;
  if (t == "margin_cap" || t == "max_margin") return d::ConstraintType::kMaxMargin;
  return d::ConstraintType::kMaxTotalNotional;
}

d::ConditionalLinkType MapLinkType(pv1::ConditionalLinkType t) {
  switch (t) {
    case pv1::CONDITIONAL_LINK_TYPE_OCO_SIBLING:   return d::ConditionalLinkType::kOco;
    case pv1::CONDITIONAL_LINK_TYPE_BRACKET_ENTRY: return d::ConditionalLinkType::kBracket;
    default:                                       return d::ConditionalLinkType::kConditional;
  }
}

std::string InstrumentSymbol(const fob::common::v1::Instrument& inst) {
  if (!inst.symbol().empty()) return inst.symbol();
  if (!inst.base().empty() && !inst.quote().empty()) return inst.base() + "/" + inst.quote();
  return inst.symbol();
}

Decimal DecimalFromDouble(double v) {
  return Decimal{static_cast<std::int64_t>(std::llround(v * 1e8)), 8};
}

d::ComboOrder BuildDomain(const pv1::CreateComboOrderRequest& req, const std::string& combo_id) {
  d::ComboOrder combo;
  combo.combo_order_id = combo_id;
  combo.user_id = req.user_id();        // T-F09-062: персистится → ledger postings
  combo.account_id = req.account_id();
  // NOTE (MVP-1): batch_order_id игнорируется — FK требует существующей строки
  // batch_orders; standalone combo не создаёт batch. Привязка к batch — позже.
  combo.batch_order_id = std::nullopt;
  combo.combo_type = MapComboType(req.combo_type());
  combo.execution_mode = MapExecMode(req.execution_mode());
  combo.atomicity_policy = MapPolicy(req.atomicity_policy());
  combo.atomicity_scope = MapScope(req.atomicity_scope());
  combo.fallback_policy = MapFallback(req.fallback_policy());
  combo.ratio_basis = MapRatioBasis(req.ratio_basis());
  if (req.has_min_execution_scale()) {
    combo.min_execution_scale = Decimal::from_proto(req.min_execution_scale());
  }
  if (req.max_ratio_deviation_bps() != 0) {
    combo.max_ratio_deviation_bps = static_cast<std::int32_t>(req.max_ratio_deviation_bps());
  }

  for (const auto& pleg : req.legs()) {
    d::Leg leg;
    leg.leg_id = pleg.leg_id().empty() ? cex::common::uuid_v4() : pleg.leg_id();
    leg.parent_order_id = combo_id;
    leg.instrument_symbol = InstrumentSymbol(pleg.instrument());
    leg.side = MapSide(pleg.side());
    leg.ratio_basis = combo.ratio_basis;
    if (pleg.has_ratio())  leg.ratio = Decimal::from_proto(pleg.ratio());
    if (pleg.has_weight()) leg.weight = Decimal::from_proto(pleg.weight());
    if (pleg.has_price_low())  leg.p_low = Decimal::from_proto(pleg.price_low());
    if (pleg.has_price_high()) leg.p_high = Decimal::from_proto(pleg.price_high());
    if (pleg.has_max_rate())   leg.q_rate = Decimal::from_proto(pleg.max_rate());
    if (pleg.has_max_qty())    leg.q_max = Decimal::from_proto(pleg.max_qty());
    if (pleg.has_filled_cum()) leg.filled_cum = Decimal::from_proto(pleg.filled_cum());
    for (const auto& vp : pleg.venue_preferences()) leg.venue_preferences.push_back(vp);
    leg.status = d::LegStatus::kActive;
    combo.legs.push_back(std::move(leg));
  }

  for (const auto& pc : req.constraints()) {
    d::MultiLegConstraint c;
    c.constraint_id = pc.constraint_id().empty() ? cex::common::uuid_v4() : pc.constraint_id();
    c.parent_order_id = combo_id;
    c.type = MapConstraintType(pc.constraint_type());
    for (const auto& [sym, coeff] : pc.coefficients()) c.coefficients[sym] = DecimalFromDouble(coeff);
    if (pc.has_lower_bound()) c.lower = Decimal::from_proto(pc.lower_bound());
    if (pc.has_upper_bound()) c.upper = Decimal::from_proto(pc.upper_bound());
    c.severity = pc.severity() == fob::orders::v1::CONSTRAINT_SEVERITY_SOFT
                     ? d::ConstraintSeverity::kSoft : d::ConstraintSeverity::kHard;
    combo.constraints.push_back(std::move(c));
  }

  for (const auto& pl : req.graph_links()) {
    d::ConditionalLink link;
    link.link_id = pl.link_id().empty() ? cex::common::uuid_v4() : pl.link_id();
    link.parent_order_id = combo_id;
    link.from_leg_id = pl.source_ref();
    link.to_leg_id = pl.target_ref();
    link.type = MapLinkType(pl.link_type());
    link.condition_json = pl.trigger_condition();
    combo.conditional_links.push_back(std::move(link));
  }

  combo.status = d::ParentOrderStatus::kActive;
  return combo;
}

// AC-F09-011 (honest-mode): фактические гарантии исполнения по режиму.
void SetGuarantees(fob::orders::v1::CreateComboOrderResponse& resp, pv1::ExecutionMode mode,
                   pv1::AtomicityScope scope) {
  if (mode == pv1::EXECUTION_MODE_ORCHESTRATION_ONLY) {
    resp.set_ratio_guaranteed(false);
    resp.set_execution_guarantees(
        "orchestration_only: ноги исполняются независимо; ratio/weights/spread НЕ гарантируются");
  } else if (scope == pv1::ATOMICITY_SCOPE_EXTERNAL_COMPENSATING) {
    resp.set_ratio_guaranteed(false);
    resp.set_execution_guarantees(
        "external_compensating: исполнение НЕ атомарно; возможна компенсирующая транзакция");
  } else {
    resp.set_ratio_guaranteed(true);
    resp.set_execution_guarantees(
        "multileg_vector_solver: ratio/weights гарантируются в пределах policy/tolerance");
  }
}

}  // namespace

CreateComboOrderUseCase::CreateComboOrderUseCase(infra::IComboOrderRepository& repository,
                                                 infra::OrdersNormalizedGroupedProducer& producer,
                                                 RiskCheckFn risk_check,
                                                 domain::ComboPolicy policy)
    : repository_(repository),
      producer_(producer),
      risk_check_(std::move(risk_check)),
      policy_(std::move(policy)) {}

fob::orders::v1::CreateComboOrderResponse CreateComboOrderUseCase::Execute(
    const fob::orders::v1::CreateComboOrderRequest& req) {
  fob::orders::v1::CreateComboOrderResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("order_flow");

  // Идемпотентность по client_combo_id (in-memory, MVP-1; durable — через
  // DB-колонку в будущем).
  const std::string client_id = req.client_combo_id();
  if (!client_id.empty()) {
    std::lock_guard<std::mutex> lk(idempotency_mu_);
    if (const auto it = idempotency_.find(client_id); it != idempotency_.end()) {
      resp.set_accepted(true);
      resp.set_combo_id(it->second);
      resp.set_status(fob::orders::v1::PARENT_ORDER_STATUS_ACTIVE);
      SetGuarantees(resp, req.execution_mode(), req.atomicity_scope());
      return resp;
    }
  }

  const std::string combo_id = cex::common::uuid_v4();
  domain::ComboOrder combo = BuildDomain(req, combo_id);

  // Валидация инвариантов + ADR-031 (multileg требует policy; strict ≠ external scope).
  if (auto err = combo.validate()) {
    resp.set_accepted(false);
    resp.set_status(fob::orders::v1::PARENT_ORDER_STATUS_REJECTED);
    auto* e = resp.mutable_error();
    e->set_code(err->code);
    e->set_message(err->message);
    return resp;
  }

  // Feature flags + лимиты (T-F09-002): честный gate режимов/типов/лимитов.
  if (auto err = policy_.CheckCreate(combo)) {
    resp.set_accepted(false);
    resp.set_status(fob::orders::v1::PARENT_ORDER_STATUS_REJECTED);
    auto* e = resp.mutable_error();
    e->set_code(err->code);
    e->set_message(err->message);
    return resp;
  }

  // Pre-trade групповой risk (hook; реальный — фаза E).
  std::string reject_reason;
  if (risk_check_ && !risk_check_(combo, reject_reason)) {
    resp.set_accepted(false);
    resp.set_status(fob::orders::v1::PARENT_ORDER_STATUS_REJECTED);
    auto* e = resp.mutable_error();
    e->set_code("RISK_REJECTED");
    e->set_message(reject_reason.empty() ? "grouped pre-trade risk rejected" : reject_reason);
    return resp;
  }

  // Persist (атомарно, 5 таблиц).
  repository_.InsertComboOrder(combo, std::nullopt);

  // orchestration_only: публикуем ноги как независимые FlowOrder.
  // multileg_vector_solver: persist-only; grouped solver matching читает из PG (MVP-2).
  if (combo.execution_mode == domain::ExecutionMode::kOrchestrationOnly) {
    cex::common::log_json("WARN", "F-09 orchestration_only: legs executed independently",
                          {{"combo_id", combo_id},
                           {"note", "target weights/ratio/spread may deviate"}});
    producer_.PublishOrchestration(combo, req.user_id(), req.account_id());
  }

  if (!client_id.empty()) {
    std::lock_guard<std::mutex> lk(idempotency_mu_);
    idempotency_[client_id] = combo_id;
  }

  resp.set_accepted(true);
  resp.set_combo_id(combo_id);
  resp.set_status(fob::orders::v1::PARENT_ORDER_STATUS_ACTIVE);
  auto& leg_ids = *resp.mutable_leg_order_ids();
  for (const auto& leg : combo.legs) {
    leg_ids[leg.leg_id] = leg.leg_id;  // orchestration_only: child FlowOrder order_id == leg_id
  }
  SetGuarantees(resp, req.execution_mode(), req.atomicity_scope());  // AC-F09-011
  return resp;
}

}  // namespace cex::order_flow::app
