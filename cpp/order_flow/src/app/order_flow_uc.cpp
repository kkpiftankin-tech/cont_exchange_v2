// =============================================================================
// Реализация OrderFlowUseCases. Здесь вся бизнес-оркестрация:
// валидация ордера, риск-чек, резерв средств, публикация события матчингу.
// =============================================================================

#include "app/order_flow_uc.hpp"

#include "cex/common/log.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"
#include "fob/orders/v1/orders.pb.h"

namespace cex::order_flow::app {

using cex::common::Decimal;

// Минимальные sanity-чеки для ордера. На проде сюда добавляются:
// проверки тиков, лот-сайза, открытых позиций пользователя и т.п.
static bool validate_order(const fob::orders::v1::FlowOrder& o, std::string& err) {
  // 1) Объём должен быть положительным.
  if (o.total_qty().units() <= 0) { err = "total_qty must be > 0"; return false; }
  // 2) Нижняя граница цены не больше верхней.
  if (Decimal::cmp(Decimal::from_proto(o.price_low()), Decimal::from_proto(o.price_high())) > 0) {
    err = "price_low must be <= price_high"; return false;
  }
  // 3) Положительная макс-скорость исполнения.
  if (o.max_speed().units() <= 0) { err = "max_speed must be > 0"; return false; }
  return true;
}

// midpoint = (a + b) / 2 в виде proto Decimal. Используется как наивная
// "опорная" цена при риск-чеке — risk нуждается в чём-то для оценки notional.
static fob::common::v1::Decimal midpoint(const fob::common::v1::Decimal& a,
                                         const fob::common::v1::Decimal& b) {
  Decimal da = Decimal::from_proto(a);
  Decimal db = Decimal::from_proto(b);
  Decimal sum = Decimal::add(da, db);
  sum.units /= 2;
  return sum.to_proto();
}

OrderFlowUseCases::OrderFlowUseCases(infra::RiskClient risk,
                                     infra::LedgerClient ledger,
                                     infra::OrdersKafkaPublisher publisher)
    : risk_(std::move(risk)),
      ledger_(std::move(ledger)),
      publisher_(std::move(publisher)) {}

// Создание flow-ордера. Поток:
//   1. валидируем входные данные;
//   2. идём в risk (синхронный gRPC) — pre-trade check;
//   3. идём в ledger — резерв средств;
//   4. сохраняем ордер в нашем кэше;
//   5. публикуем OrdersNormalized.create в Kafka.
// При любой ошибке на шагах 1-3 возвращаем accepted=false с error.
fob::orders::v1::CreateFlowOrderResponse OrderFlowUseCases::CreateFlowOrder(
    const fob::orders::v1::CreateFlowOrderRequest& req) {
  fob::orders::v1::CreateFlowOrderResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("order_flow");

  // 1) Валидация входа.
  std::string err;
  if (!validate_order(req.order(), err)) {
    resp.set_accepted(false);
    auto* e = resp.mutable_error();
    e->set_code("VALIDATION_ERROR");
    e->set_message(err);
    return resp;
  }

  // 2) Risk pre-trade. Передаём опорную цену, иначе risk не сможет посчитать notional.
  fob::risk::v1::PreTradeCheckRequest risk_req;
  *risk_req.mutable_meta() = req.meta();
  risk_req.set_user_id(req.order().user_id());
  *risk_req.mutable_order() = req.order();
  *risk_req.mutable_reference_price() = midpoint(req.order().price_low(), req.order().price_high());

  auto risk_resp = risk_.CheckNewOrder(risk_req);
  if (risk_resp.decision() != fob::risk::v1::RISK_DECISION_ACCEPT) {
    // Reject или review — наружу выходит как ошибка. error из risk-ответа сохраняется.
    resp.set_accepted(false);
    *resp.mutable_error() = risk_resp.error();
    return resp;
  }

  // 3) Резерв средств. Логика зависит от стороны:
  //    BUY  -> резервируем quote под максимально возможную стоимость (price_high).
  //            Если на матчинге цена окажется ниже, разница будет освобождена.
  //    SELL -> резервируем base в полном объёме total_qty.
  fob::ledger::v1::ReserveFundsRequest led_req;
  *led_req.mutable_meta() = req.meta();
  led_req.set_reservation_id(req.order().order_id()); // идемпотентность по order_id
  led_req.set_user_id(req.order().user_id());
  led_req.set_order_id(req.order().order_id());

  if (req.order().side() == fob::common::v1::SIDE_BUY) {
    led_req.set_currency(req.order().instrument().quote());
    Decimal qty   = Decimal::from_proto(req.order().total_qty());
    Decimal price = Decimal::from_proto(req.order().price_high());
    Decimal notional = Decimal::mul(qty, price);
    *led_req.mutable_amount() = notional.to_proto();
    led_req.set_reason(fob::ledger::v1::RESERVE_REASON_NEW_ORDER);
  } else if (req.order().side() == fob::common::v1::SIDE_SELL) {
    led_req.set_currency(req.order().instrument().base());
    *led_req.mutable_amount() = req.order().total_qty();
    led_req.set_reason(fob::ledger::v1::RESERVE_REASON_NEW_ORDER);
  } else {
    // SIDE_UNSPECIFIED / неподдерживаемое значение — это ошибка клиента.
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

  // 4) Сохраняем у себя текущее состояние ордера.
  fob::orders::v1::FlowOrder stored = req.order();
  stored.set_status(fob::common::v1::ORDER_STATUS_NEW);
  *stored.mutable_remaining_qty() = stored.total_qty();
  *stored.mutable_updated_at() = cex::common::now_ts();
  orders_[stored.order_id()] = stored;

  // 5) Публикуем нормализованное событие в Kafka — это вход для матчинга.
  fob::orders::v1::OrdersNormalized evt;
  auto* meta = evt.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("order_flow");
  meta->set_correlation_id(req.meta().correlation_id());
  // Партиционирование по символу — гарантирует, что все события одной пары
  // приходят в одну партицию и обрабатываются по порядку.
  meta->set_partition_key(stored.instrument().symbol());

  auto* create = evt.mutable_create();
  *create->mutable_order() = stored;

  publisher_.publish(evt);

  resp.set_accepted(true);
  resp.set_order_id(stored.order_id());
  return resp;
}

// Отмена ордера. NOT_FOUND если у нас нет такого id.
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

  // Локальное состояние в кэше.
  it->second.set_status(fob::common::v1::ORDER_STATUS_CANCELED);
  *it->second.mutable_updated_at() = cex::common::now_ts();

  // Сообщаем матчингу — он перестанет выдавать fill'ы по этому ордеру.
  fob::orders::v1::OrdersNormalized evt;
  auto* meta = evt.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("order_flow");
  meta->set_correlation_id(req.meta().correlation_id());
  meta->set_partition_key(it->second.instrument().symbol());

  auto* cancel = evt.mutable_cancel();
  cancel->set_order_id(req.order_id());
  cancel->set_user_id(req.user_id());
  cancel->set_reason(req.reason());

  publisher_.publish(evt);

  // Снимаем резерв в ledger'е.
  // ВАЖНО (TODO): сейчас мы не помним точную валюту/сумму резервации, поэтому
  // отправляем заглушки. В проде надо хранить пары (order_id -> reservation_meta)
  // в БД и снимать ровно ту сумму, что блокировали. Сервер ledger найдёт резерв
  // по reservation_id (он же order_id).
  fob::ledger::v1::ReleaseFundsRequest rel;
  *rel.mutable_meta() = req.meta();
  rel.set_reservation_id(req.order_id());
  rel.set_user_id(req.user_id());
  rel.set_order_id(req.order_id());
  rel.set_reason("cancel");
  rel.set_currency("UNKNOWN");
  rel.mutable_amount()->set_units(0);
  rel.mutable_amount()->set_scale(0);
  ledger_.ReleaseFunds(rel);

  resp.set_success(true);
  return resp;
}

// Чтение состояния ордера. Только из локального кэша — другие сервисы (matching,
// ledger) могут иметь более свежую информацию, но в MVP этого достаточно.
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

}  // namespace cex::order_flow::app
