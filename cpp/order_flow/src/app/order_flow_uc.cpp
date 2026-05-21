#include "app/order_flow_uc.hpp"

#include <algorithm>
#include <vector>

#include "cex/common/log.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"
#include "fob/orders/v1/orders.pb.h"

namespace cex::order_flow::app {

using cex::common::Decimal;

static bool validate_order(const fob::orders::v1::FlowOrder& o, std::string& err) {
  // Basic sanity checks (MVP):
  // 1) total_qty > 0
  // 2) price_low <= price_high
  // 3) max_speed > 0
  if (o.total_qty().units() <= 0) { err = "total_qty must be > 0"; return false; }
  if (Decimal::cmp(Decimal::from_proto(o.price_low()), Decimal::from_proto(o.price_high())) > 0) {
    err = "price_low must be <= price_high"; return false;
  }
  if (o.max_speed().units() <= 0) { err = "max_speed must be > 0"; return false; }
  return true;
}

static fob::common::v1::Decimal midpoint(const fob::common::v1::Decimal& a,
                                         const fob::common::v1::Decimal& b) {
  // midpoint = (a + b) / 2
  // We do it in fixed-point: align scales, add units, divide by 2.
  Decimal da = Decimal::from_proto(a);
  Decimal db = Decimal::from_proto(b);
  Decimal sum = Decimal::add(da, db);
  sum.units /= 2;
  return sum.to_proto();
}

static int64_t timestamp_to_unix_ms(const google::protobuf::Timestamp& ts) {
  return ts.seconds() * 1000 + ts.nanos() / 1000000;
}

OrderFlowUseCases::OrderFlowUseCases(infra::RiskClient risk,
                                     infra::LedgerClient ledger,
                                     infra::OrdersKafkaPublisher publisher)
    : risk_(std::move(risk)),
      ledger_(std::move(ledger)),
      publisher_(std::move(publisher)) {}

fob::orders::v1::CreateFlowOrderResponse OrderFlowUseCases::CreateFlowOrder(
    const fob::orders::v1::CreateFlowOrderRequest& req) {
  fob::orders::v1::CreateFlowOrderResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("order_flow");

  // 1) Validate input
  std::string err;
  if (!validate_order(req.order(), err)) {
    resp.set_accepted(false);
    auto* e = resp.mutable_error();
    e->set_code("VALIDATION_ERROR");
    e->set_message(err);
    return resp;
  }

  // 2) Call Risk (pre-trade)
  fob::risk::v1::PreTradeCheckRequest risk_req;
  *risk_req.mutable_meta() = req.meta();
  risk_req.set_user_id(req.order().user_id());
  *risk_req.mutable_order() = req.order();

  // For MVP we provide a naive reference price = midpoint(price_low, price_high).
  *risk_req.mutable_reference_price() = midpoint(req.order().price_low(), req.order().price_high());

  auto risk_resp = risk_.CheckNewOrder(risk_req);
  if (risk_resp.decision() == fob::risk::v1::RISK_DECISION_HALT ||
      risk_resp.decision() == fob::risk::v1::RISK_DECISION_REJECT) {
    resp.set_accepted(false);
    *resp.mutable_error() = risk_resp.error();
    return resp;
  }
  fob::orders::v1::FlowOrder approved_order = req.order();
  if (risk_resp.decision() == fob::risk::v1::RISK_DECISION_RESIZE &&
      risk_resp.has_resized_order()) {
    approved_order = risk_resp.resized_order();
  }

  // 3) Reserve funds in Ledger
  // BUY: reserve quote = total_qty * price_high (worst-case).
  // SELL: reserve base = total_qty
  fob::ledger::v1::ReserveFundsRequest led_req;
  *led_req.mutable_meta() = req.meta();
  led_req.set_reservation_id(approved_order.order_id()); // idempotency: order id
  led_req.set_user_id(approved_order.user_id());
  led_req.set_order_id(approved_order.order_id());

  if (approved_order.side() == fob::common::v1::SIDE_BUY) {
    led_req.set_currency(approved_order.instrument().quote());
    Decimal qty = Decimal::from_proto(approved_order.total_qty());
    Decimal price = Decimal::from_proto(approved_order.price_high());
    Decimal notional = Decimal::mul(qty, price);
    *led_req.mutable_amount() = notional.to_proto();
    led_req.set_reason(fob::ledger::v1::RESERVE_REASON_NEW_ORDER);
  } else if (approved_order.side() == fob::common::v1::SIDE_SELL) {
    led_req.set_currency(approved_order.instrument().base());
    *led_req.mutable_amount() = approved_order.total_qty();
    led_req.set_reason(fob::ledger::v1::RESERVE_REASON_NEW_ORDER);
  } else {
    resp.set_accepted(false);
    auto* e = resp.mutable_error();
    e->set_code("SIDE_UNSPECIFIED");
    e->set_message("Order side must be BUY or SELL");
    return resp;
  }

  auto led_resp = ledger_.ReserveFunds(led_req);
  if (!led_resp.success()) {
    resp.set_accepted(false);
    *resp.mutable_error() = led_resp.error();
    return resp;
  }

  // 4) Persist order in memory (MVP)
  fob::orders::v1::FlowOrder stored = approved_order;
  stored.set_status(fob::common::v1::ORDER_STATUS_NEW);
  *stored.mutable_remaining_qty() = stored.total_qty();
  *stored.mutable_updated_at() = cex::common::now_ts();

  orders_[stored.order_id()] = stored;

  // 5) Publish event to Kafka for matching engine
  fob::orders::v1::OrdersNormalized evt;
  auto* meta = evt.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("order_flow");
  meta->set_correlation_id(req.meta().correlation_id());
  meta->set_partition_key(stored.instrument().symbol()); // partition by symbol for matching locality

  auto* create = evt.mutable_create();
  *create->mutable_order() = stored;

  publisher_.publish(evt);

  resp.set_accepted(true);
  resp.set_order_id(stored.order_id());
  return resp;
}

fob::orders::v1::CancelFlowOrderResponse OrderFlowUseCases::CancelFlowOrder(
    const fob::orders::v1::CancelFlowOrderRequest& req) {
  fob::orders::v1::CancelFlowOrderResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("order_flow");

  auto it = orders_.find(req.order_id());
  if (it == orders_.end()) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("NOT_FOUND");
    e->set_message("Order not found");
    return resp;
  }

  // Update in-memory state
  it->second.set_status(fob::common::v1::ORDER_STATUS_CANCELED);
  *it->second.mutable_updated_at() = cex::common::now_ts();

  // Publish cancel to Kafka so matching can stop it.
  fob::orders::v1::OrdersNormalized evt;
  auto* meta = evt.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("order_flow");
  meta->set_correlation_id(req.meta().correlation_id());
  meta->set_partition_key(it->second.instrument().symbol());

  auto* cancel = evt.mutable_cancel();
  cancel->set_order_id(req.order_id());
  cancel->set_user_id(req.user_id().empty() ? it->second.user_id() : req.user_id());
  cancel->set_reason(req.reason());

  publisher_.publish(evt);

  // Ledger tracks reservation amounts by reservation_id, so release by order_id.
  fob::ledger::v1::ReleaseFundsRequest rel;
  *rel.mutable_meta() = req.meta();
  rel.set_reservation_id(req.order_id());
  rel.set_user_id(req.user_id().empty() ? it->second.user_id() : req.user_id());
  rel.set_order_id(req.order_id());
  rel.set_reason(req.reason().empty() ? "cancel" : req.reason());
  ledger_.ReleaseFunds(rel);

  resp.set_success(true);
  return resp;
}

fob::orders::v1::GetFlowOrderResponse OrderFlowUseCases::GetFlowOrder(
    const fob::orders::v1::GetFlowOrderRequest& req) {
  fob::orders::v1::GetFlowOrderResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("order_flow");

  auto it = orders_.find(req.order_id());
  if (it == orders_.end()) {
    auto* v = resp.mutable_view();
    auto* e = v->mutable_error();
    e->set_code("NOT_FOUND");
    e->set_message("Order not found");
    return resp;
  }

  *resp.mutable_view()->mutable_order() = it->second;
  return resp;
}

fob::orders::v1::ListFlowOrdersResponse OrderFlowUseCases::ListFlowOrders(
    const fob::orders::v1::ListFlowOrdersRequest& req) {
  fob::orders::v1::ListFlowOrdersResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("order_flow");

  std::vector<const fob::orders::v1::FlowOrder*> filtered_orders;
  filtered_orders.reserve(orders_.size());
  for (const auto& [order_id, order] : orders_) {
    (void)order_id;
    if (!req.user_id().empty() && order.user_id() != req.user_id()) {
      continue;
    }
    filtered_orders.push_back(&order);
  }

  std::sort(filtered_orders.begin(), filtered_orders.end(),
            [](const fob::orders::v1::FlowOrder* left,
               const fob::orders::v1::FlowOrder* right) {
              return timestamp_to_unix_ms(left->created_at()) >
                     timestamp_to_unix_ms(right->created_at());
            });

  for (const auto* order : filtered_orders) {
    *resp.add_views()->mutable_order() = *order;
  }
  return resp;
}

}  // namespace cex::order_flow::app
