#include "app/order_flow_uc.hpp"

#include <algorithm>
#include <exception>
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
                                     infra::OrdersKafkaPublisher publisher,
                                     std::shared_ptr<infra::IFlowOrderRepository> flow_order_repo)
    : risk_(std::move(risk)),
      ledger_(std::move(ledger)),
      publisher_(std::move(publisher)),
      flow_order_repo_(std::move(flow_order_repo)) {}

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

  {
    std::lock_guard<std::mutex> lock(orders_mu_);
    orders_[stored.order_id()] = stored;
  }

  // 4b) Persist to PostgreSQL flow_orders so F-04 matching picks the order
  // up. Without this, matching's PostgresFlowOrderRepository finds no rows
  // and the order is silently dropped from batch clearing (IN-007 gap
  // "order-flow-postgres-write-pending"). Status is written as 'active' to
  // match matching's WHERE filter status IN ('active','partially_filled').
  // When no DSN is configured (dev / legacy compose), flow_order_repo_ is
  // null and we keep the in-memory + Kafka-only path.
  if (flow_order_repo_) {
    try {
      flow_order_repo_->InsertFlowOrder(stored);
    } catch (const std::exception& e) {
      // Reservation already succeeded; rolling back here would require
      // releasing the reserve, which the cancel path handles. For MVP, log
      // and fail the request — caller retries with the same client order id.
      cex::common::log_json("ERROR",
                            "OrderFlow: failed to persist FlowOrder to PostgreSQL",
                            {{"order_id", stored.order_id()},
                             {"user_id", stored.user_id()},
                             {"error", e.what()}});
      // Release reserve to avoid leaking funds.
      fob::ledger::v1::ReleaseFundsRequest rel;
      *rel.mutable_meta() = req.meta();
      rel.set_reservation_id(stored.order_id());
      rel.set_user_id(stored.user_id());
      rel.set_order_id(stored.order_id());
      rel.set_reason("persist_failed");
      ledger_.ReleaseFunds(rel);
      // Drop the in-memory entry as well.
      {
        std::lock_guard<std::mutex> lock(orders_mu_);
        orders_.erase(stored.order_id());
      }
      resp.set_accepted(false);
      auto* re = resp.mutable_error();
      re->set_code("PERSIST_ERROR");
      re->set_message(e.what());
      return resp;
    }
  }

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

  std::string symbol_for_partition;
  std::string user_id_for_cancel;
  {
    std::lock_guard<std::mutex> lock(orders_mu_);
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
    symbol_for_partition = it->second.instrument().symbol();
    user_id_for_cancel = it->second.user_id();
  }

  // Publish cancel to Kafka so matching can stop it.
  fob::orders::v1::OrdersNormalized evt;
  auto* meta = evt.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("order_flow");
  meta->set_correlation_id(req.meta().correlation_id());
  meta->set_partition_key(symbol_for_partition);

  auto* cancel = evt.mutable_cancel();
  cancel->set_order_id(req.order_id());
  cancel->set_user_id(req.user_id().empty() ? user_id_for_cancel : req.user_id());
  cancel->set_reason(req.reason());

  publisher_.publish(evt);

  // Ledger tracks reservation amounts by reservation_id, so release by order_id.
  fob::ledger::v1::ReleaseFundsRequest rel;
  *rel.mutable_meta() = req.meta();
  rel.set_reservation_id(req.order_id());
  rel.set_user_id(req.user_id().empty() ? user_id_for_cancel : req.user_id());
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

  std::lock_guard<std::mutex> lock(orders_mu_);
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

  // Snapshot copy under lock; sort/serialize without holding it.
  std::vector<fob::orders::v1::FlowOrder> snapshot;
  {
    std::lock_guard<std::mutex> lock(orders_mu_);
    snapshot.reserve(orders_.size());
    for (const auto& [order_id, order] : orders_) {
      (void)order_id;
      if (!req.user_id().empty() && order.user_id() != req.user_id()) {
        continue;
      }
      snapshot.push_back(order);
    }
  }

  std::sort(snapshot.begin(), snapshot.end(),
            [](const fob::orders::v1::FlowOrder& left,
               const fob::orders::v1::FlowOrder& right) {
              return timestamp_to_unix_ms(left.created_at()) >
                     timestamp_to_unix_ms(right.created_at());
            });

  for (auto& order : snapshot) {
    *resp.add_views()->mutable_order() = std::move(order);
  }
  return resp;
}

void OrderFlowUseCases::ApplyBatchResult(
    const fob::matching::v1::BatchResult& batch) {
  std::lock_guard<std::mutex> lock(orders_mu_);

  int applied = 0;
  int duplicates = 0;
  int unknown = 0;

  // 1) Apply per-order fills as decrements to remaining_qty.
  for (const auto& fill : batch.fills()) {
    const std::string idem_key = batch.batch_id() + "|" + fill.order_id();
    if (applied_fills_.find(idem_key) != applied_fills_.end()) {
      ++duplicates;
      continue;  // already applied earlier
    }

    auto it = orders_.find(fill.order_id());
    if (it == orders_.end()) {
      ++unknown;
      continue;
    }

    auto& order = it->second;
    if (order.status() == fob::common::v1::ORDER_STATUS_CANCELED ||
        order.status() == fob::common::v1::ORDER_STATUS_EXPIRED ||
        order.status() == fob::common::v1::ORDER_STATUS_REJECTED) {
      // Terminal status: don't reactivate. Still mark as applied so we don't
      // count again on replay.
      applied_fills_[idem_key] = true;
      continue;
    }

    const Decimal remaining = Decimal::from_proto(order.remaining_qty());
    const Decimal executed = Decimal::from_proto(fill.executed_qty());
    Decimal new_remaining = Decimal::sub(remaining, executed);
    if (new_remaining.units < 0) {
      new_remaining.units = 0;  // hard floor — see BUG-2 (overfill defence)
    }
    *order.mutable_remaining_qty() = new_remaining.to_proto();

    if (new_remaining.units == 0) {
      order.set_status(fob::common::v1::ORDER_STATUS_FILLED);
    } else {
      order.set_status(fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED);
    }
    *order.mutable_updated_at() = cex::common::now_ts();

    applied_fills_[idem_key] = true;
    ++applied;
  }

  // 2) Process explicit OrderStateUpdate entries: matching may send cumulative
  // state which is authoritative. We only respect updates for orders we own
  // and never overwrite a terminal cancellation by the user.
  for (const auto& update : batch.order_updates()) {
    auto it = orders_.find(update.order_id());
    if (it == orders_.end()) continue;
    auto& order = it->second;
    if (order.status() == fob::common::v1::ORDER_STATUS_CANCELED) continue;
    if (update.has_remaining_qty()) {
      *order.mutable_remaining_qty() = update.remaining_qty();
    }
    if (update.status() != fob::common::v1::ORDER_STATUS_UNSPECIFIED) {
      order.set_status(update.status());
    }
    *order.mutable_updated_at() = cex::common::now_ts();
  }

  if (applied > 0 || duplicates > 0 || unknown > 0) {
    cex::common::log_json("INFO",
                          "OrderFlow applied batch result",
                          {{"batch_id", batch.batch_id()},
                           {"fills_applied", std::to_string(applied)},
                           {"fills_duplicate", std::to_string(duplicates)},
                           {"fills_unknown_order", std::to_string(unknown)},
                           {"order_updates", std::to_string(batch.order_updates_size())}});
  }
}

}  // namespace cex::order_flow::app
