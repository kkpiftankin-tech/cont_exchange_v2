// ============================================================================
// orders_normalized_grouped_producer.cpp — F-09 (T-F09-035). См. .hpp.
// ============================================================================

#include "infra/orders_normalized_grouped_producer.hpp"

#include <stdexcept>

#include "cex/common/time.hpp"   // now_ts()
#include "cex/common/uuid.hpp"   // uuid_v4()
#include "fob/common/v1/common.pb.h"

namespace cex::order_flow::infra {

namespace {

namespace d = cex::order_flow::domain;

fob::common::v1::Side SideProto(d::Side s) {
  switch (s) {
    case d::Side::kBuy:  return fob::common::v1::SIDE_BUY;
    case d::Side::kSell: return fob::common::v1::SIDE_SELL;
    default:             return fob::common::v1::SIDE_UNSPECIFIED;
  }
}

std::string ExecModeTag(d::ExecutionMode m) {
  return m == d::ExecutionMode::kMultilegVectorSolver ? "multileg_vector_solver"
                                                      : "orchestration_only";
}

std::string AtomicityTag(d::AtomicityPolicy p) {
  switch (p) {
    case d::AtomicityPolicy::kStrictAtomic:        return "strict_atomic";
    case d::AtomicityPolicy::kScalableAtomic:      return "scalable_atomic";
    case d::AtomicityPolicy::kBestEffort:          return "best_effort";
    case d::AtomicityPolicy::kSequentialFallback:  return "sequential_fallback";
    case d::AtomicityPolicy::kExternalCompensating:return "external_compensating";
    default:                                       return "best_effort";
  }
}

std::string ComboTypeTag(d::ComboType t) {
  switch (t) {
    case d::ComboType::kPair: return "pair";
    case d::ComboType::kBasket: return "basket";
    case d::ComboType::kSpread: return "spread";
    case d::ComboType::kConditional: return "conditional";
    case d::ComboType::kOco: return "oco";
    case d::ComboType::kBracket: return "bracket";
    case d::ComboType::kFactor: return "factor";
    case d::ComboType::kBudget: return "budget";
    default: return "combo";
  }
}

}  // namespace

OrdersNormalizedGroupedProducer::OrdersNormalizedGroupedProducer(PublishFn publish)
    : publish_(std::move(publish)) {
  if (!publish_) {
    throw std::invalid_argument("OrdersNormalizedGroupedProducer requires a publish function");
  }
}

bool OrdersNormalizedGroupedProducer::PublishOrchestration(const domain::ComboOrder& combo,
                                                           const std::string& user_id,
                                                           const std::string& account_id) {
  if (combo.execution_mode != domain::ExecutionMode::kOrchestrationOnly) {
    throw std::invalid_argument(
        "PublishOrchestration: only orchestration_only is supported (solver path is MVP-2)");
  }
  if (auto err = combo.validate()) {
    throw std::invalid_argument("PublishOrchestration: invalid combo: " + err->message);
  }

  bool all_ok = true;
  for (const auto& leg : combo.legs) {
    fob::orders::v1::OrdersNormalized evt;

    auto* meta = evt.mutable_meta();
    meta->set_event_id(cex::common::uuid_v4());
    *meta->mutable_ts_event() = cex::common::now_ts();
    meta->set_source("order_flow");
    meta->set_correlation_id(combo.combo_order_id);
    // AC-F09-035: все ноги одной combo → один partition (key = parentOrderId).
    meta->set_partition_key(combo.combo_order_id);

    auto* order = evt.mutable_create()->mutable_order();
    order->set_order_id(leg.leg_id);
    order->set_user_id(user_id);
    order->set_account_id(account_id);
    order->mutable_instrument()->set_symbol(leg.instrument_symbol);
    order->set_side(SideProto(leg.side));
    *order->mutable_total_qty() = leg.q_max.to_proto();
    *order->mutable_remaining_qty() = leg.remaining_qty().to_proto();
    *order->mutable_price_low() = leg.p_low.to_proto();
    *order->mutable_price_high() = leg.p_high.to_proto();
    *order->mutable_max_speed() = leg.q_rate.to_proto();
    order->set_tif(fob::common::v1::TIF_GTC);

    // Группирующие метаданные — matching и downstream могут связать ногу с
    // родителем, не ломая FlowOrder-контракт (backward-compatible).
    auto& tags = *order->mutable_tags();
    tags["combo_id"] = combo.combo_order_id;
    tags["leg_id"] = leg.leg_id;
    tags["execution_mode"] = ExecModeTag(combo.execution_mode);
    tags["atomicity_policy"] = AtomicityTag(combo.atomicity_policy);
    tags["combo_type"] = ComboTypeTag(combo.combo_type);

    all_ok = publish_(evt) && all_ok;
  }
  return all_ok;
}

bool OrdersNormalizedGroupedProducer::PublishLegCancel(const std::string& combo_order_id,
                                                       const std::string& leg_id,
                                                       const std::string& user_id,
                                                       const std::string& reason) {
  fob::orders::v1::OrdersNormalized evt;
  auto* meta = evt.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("order_flow");
  meta->set_correlation_id(combo_order_id);
  meta->set_partition_key(combo_order_id);  // та же партиция, что у create

  auto* cancel = evt.mutable_cancel();
  cancel->set_order_id(leg_id);  // дочерний FlowOrder.order_id == leg_id
  cancel->set_user_id(user_id);
  cancel->set_reason(reason);

  return publish_(evt);
}

}  // namespace cex::order_flow::infra
