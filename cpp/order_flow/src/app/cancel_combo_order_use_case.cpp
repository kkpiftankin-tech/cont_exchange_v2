// ============================================================================
// cancel_combo_order_use_case.cpp — F-09 (T-F09-033). См. .hpp.
// ============================================================================

#include "app/cancel_combo_order_use_case.hpp"

#include "fob/common/v1/common.pb.h"

namespace cex::order_flow::app {

namespace {

namespace d = cex::order_flow::domain;
namespace pv1 = fob::orders::v1;

pv1::ParentOrderStatus StatusProto(d::ParentOrderStatus s) {
  switch (s) {
    case d::ParentOrderStatus::kActive: return pv1::PARENT_ORDER_STATUS_ACTIVE;
    case d::ParentOrderStatus::kWaitingForTrigger: return pv1::PARENT_ORDER_STATUS_WAITING_FOR_TRIGGER;
    case d::ParentOrderStatus::kPartiallyFilled: return pv1::PARENT_ORDER_STATUS_PARTIALLY_FILLED;
    case d::ParentOrderStatus::kFilled: return pv1::PARENT_ORDER_STATUS_FILLED;
    case d::ParentOrderStatus::kCancelled: return pv1::PARENT_ORDER_STATUS_CANCELLED;
    case d::ParentOrderStatus::kExpired: return pv1::PARENT_ORDER_STATUS_EXPIRED;
    case d::ParentOrderStatus::kDegraded: return pv1::PARENT_ORDER_STATUS_DEGRADED;
    case d::ParentOrderStatus::kRollbackPending: return pv1::PARENT_ORDER_STATUS_ROLLBACK_PENDING;
    case d::ParentOrderStatus::kRolledback: return pv1::PARENT_ORDER_STATUS_ROLLEDBACK;
    case d::ParentOrderStatus::kRejected: return pv1::PARENT_ORDER_STATUS_REJECTED;
    case d::ParentOrderStatus::kRiskPending: return pv1::PARENT_ORDER_STATUS_RISK_PENDING;
    default: return pv1::PARENT_ORDER_STATUS_DRAFT;
  }
}

bool IsTerminal(d::ParentOrderStatus s) {
  return s == d::ParentOrderStatus::kCancelled || s == d::ParentOrderStatus::kFilled ||
         s == d::ParentOrderStatus::kExpired || s == d::ParentOrderStatus::kRejected ||
         s == d::ParentOrderStatus::kRolledback;
}

}  // namespace

CancelComboOrderUseCase::CancelComboOrderUseCase(infra::IComboOrderRepository& repository,
                                                 infra::OrdersNormalizedGroupedProducer& producer)
    : repository_(repository), producer_(producer) {}

fob::orders::v1::CancelComboOrderResponse CancelComboOrderUseCase::Execute(
    const fob::orders::v1::CancelComboOrderRequest& req) {
  fob::orders::v1::CancelComboOrderResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("order_flow");

  const std::string combo_id = req.combo_id();
  if (combo_id.empty()) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("INVALID_ARGUMENT");
    e->set_message("combo_id is empty");
    return resp;
  }

  const auto status = repository_.GetComboStatus(combo_id);
  if (!status.has_value()) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("COMBO_NOT_FOUND");
    e->set_message("combo not found: " + combo_id);
    return resp;
  }

  // Идемпотентность: уже-терминальная заявка → success без side-effects.
  if (IsTerminal(*status)) {
    resp.set_success(true);
    resp.set_status(StatusProto(*status));
    return resp;
  }

  // Снимаем дочерние FlowOrder в matching (до изменения статусов в PG — но
  // оба идемпотентны, порядок не критичен).
  const auto leg_ids = repository_.GetActiveLegIds(combo_id);
  repository_.CancelComboAndLegs(combo_id);
  for (const auto& leg_id : leg_ids) {
    producer_.PublishLegCancel(combo_id, leg_id, req.user_id(), req.reason());
  }

  resp.set_success(true);
  resp.set_status(fob::orders::v1::PARENT_ORDER_STATUS_CANCELLED);
  return resp;
}

}  // namespace cex::order_flow::app
