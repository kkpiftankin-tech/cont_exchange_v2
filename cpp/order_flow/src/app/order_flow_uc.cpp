// ============================================================================
// order_flow_uc.cpp — application use cases сервиса order_flow.
//
// Назначение:
//   order_flow — пользовательский edge для FlowOrder lifecycle. Принимает
//   gRPC create/cancel/get/list, прогоняет pre-trade risk, резервирует
//   средства в ledger, персистит заявку в PG (PR-F02-001), и публикует
//   событие в Kafka orders.normalized для matching-сервиса.
//
//   Use cases:
//     * CreateFlowOrder  — главный flow F-02: validate → risk → reserve →
//                          PG persist → Kafka publish.
//     * CancelFlowOrder  — F-03: mark cancelled in-memory + Kafka cancel
//                          event + release reservation.
//     * GetFlowOrder     — F-02 view: lookup по order_id.
//     * ListFlowOrders   — F-02 view: список по user_id, sorted by created_at.
//     * ApplyBatchResult — async обновление remaining_qty и status по fills
//                          из batch.outputs (вызывается consumer'ом).
//
// Контракт и инварианты:
//   * Все money через cex::common::Decimal (CLAUDE.md §9).
//   * Идempotency по order_id: повторный CreateFlowOrder ON CONFLICT DO NOTHING.
//   * BUY резервирует quote = total_qty * price_high (worst-case).
//   * SELL резервирует base = total_qty.
//   * Reservation rollback: если PG INSERT fails — ReleaseFunds вызывается.
//   * ApplyBatchResult idempotency через applied_fills_ set (key: batch+order).
//
// Concurrency:
//   * orders_ защищён orders_mu_ (std::mutex). Все mutate-операции под lock.
//   * Snapshot-copy + работа вне lock'а для long-running операций (sort, serialize).
// ============================================================================

#include "app/order_flow_uc.hpp"

#include <algorithm>     // std::sort для ListFlowOrders
#include <exception>     // std::exception::what() в catch
#include <vector>        // snapshot pattern в ListFlowOrders

#include "cex/common/log.hpp"          // structured JSON logging
#include "cex/common/time.hpp"         // now_ts() — proto Timestamp с текущим временем
#include "cex/common/uuid.hpp"         // uuid_v4 для event_id
#include "fob/orders/v1/orders.pb.h"   // generated proto

namespace cex::order_flow::app {

// Удобный alias для частого использования.
using cex::common::Decimal;

// ----------------------------------------------------------------------------
// validate_order — базовые sanity-чеки FlowOrder (MVP-уровень).
// Полные инварианты (CLAUDE.md §8.2) проверяются в domain слое; здесь —
// быстрый gate перед дорогими операциями (Risk RPC + Ledger reserve).
//
// Возвращает true/false и заполняет err сообщением при false.
// Pass-by-reference для err — изменяемый out-параметр.
// ----------------------------------------------------------------------------
static bool validate_order(const fob::orders::v1::FlowOrder& o, std::string& err) {
  // Basic sanity checks (MVP):
  // 1) total_qty > 0
  // 2) price_low <= price_high
  // 3) max_speed > 0

  // Проверка raw units > 0 — достаточно, потому что Decimal.scale всегда >=0,
  // и знак определяется только знаком units.
  if (o.total_qty().units() <= 0) { err = "total_qty must be > 0"; return false; }
  // Сравнение Decimal через cmp учитывает разные scale полей price_low/high.
  // > 0 = price_low > price_high → нарушение инварианта (CLAUDE.md §8.2 (6)).
  if (Decimal::cmp(Decimal::from_proto(o.price_low()), Decimal::from_proto(o.price_high())) > 0) {
    err = "price_low must be <= price_high"; return false;
  }
  if (o.max_speed().units() <= 0) { err = "max_speed must be > 0"; return false; }
  return true;
}

// ----------------------------------------------------------------------------
// midpoint — (price_low + price_high) / 2, для naive risk reference price.
// Используется как простой proxy "разумной" цены в pre-trade check.
// ----------------------------------------------------------------------------
/// Физический смысл: средняя точка между нижней и верхней границами цены
/// заявки. В реальной системе reference_price берётся из MarketData
/// (last trade / mid book), но для MVP это упрощение работает.
static fob::common::v1::Decimal midpoint(const fob::common::v1::Decimal& a,
                                         const fob::common::v1::Decimal& b) {
  // midpoint = (a + b) / 2
  // We do it in fixed-point: align scales, add units, divide by 2.
  Decimal da = Decimal::from_proto(a);
  Decimal db = Decimal::from_proto(b);
  Decimal sum = Decimal::add(da, db);
  // Integer division: для odd sum.units получим floor — небольшая потеря
  // точности (1/2 unit), приемлемо для reference_price.
  sum.units /= 2;
  return sum.to_proto();
}

/// Конвертация proto.Timestamp → unix ms (для sort). nanos точность не нужна.
static int64_t timestamp_to_unix_ms(const google::protobuf::Timestamp& ts) {
  return ts.seconds() * 1000 + ts.nanos() / 1000000;
}

// ----------------------------------------------------------------------------
// Конструктор. Принимает зависимости через value (move) — typical DI pattern.
// flow_order_repo — shared_ptr потому что nullable (без DSN → nullptr,
// без перенастройки).
// ----------------------------------------------------------------------------
OrderFlowUseCases::OrderFlowUseCases(infra::RiskClient risk,
                                     infra::LedgerClient ledger,
                                     infra::OrdersKafkaPublisher publisher,
                                     std::shared_ptr<infra::IFlowOrderRepository> flow_order_repo)
    : risk_(std::move(risk)),
      ledger_(std::move(ledger)),
      publisher_(std::move(publisher)),
      flow_order_repo_(std::move(flow_order_repo)) {}

// ============================================================================
// CreateFlowOrder — F-02 главный flow.
//
// Шаги (важен порядок: каждый шаг откатывается предыдущим при failure):
//   1. Validate input (no side-effects).
//   2. Risk pre-trade check (no side-effects).
//   3. Ledger ReserveFunds (CAN BE ROLLED BACK via ReleaseFunds).
//   4. In-memory persist.
//   5. PG persist (если flow_order_repo_ есть). Failure → ReleaseFunds + erase in-mem.
//   6. Kafka publish orders.normalized.create.
// ============================================================================
fob::orders::v1::CreateFlowOrderResponse OrderFlowUseCases::CreateFlowOrder(
    const fob::orders::v1::CreateFlowOrderRequest& req) {
  fob::orders::v1::CreateFlowOrderResponse resp;
  // Echo meta с обновлённым source — стандартный pattern для traceability.
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
  // Conformance с CLAUDE.md §16: pre-trade checks ДО активации заявки.
  fob::risk::v1::PreTradeCheckRequest risk_req;
  *risk_req.mutable_meta() = req.meta();
  risk_req.set_user_id(req.order().user_id());
  *risk_req.mutable_order() = req.order();

  // For MVP we provide a naive reference price = midpoint(price_low, price_high).
  *risk_req.mutable_reference_price() = midpoint(req.order().price_low(), req.order().price_high());

  auto risk_resp = risk_.CheckNewOrder(risk_req);
  // HALT и REJECT — terminal failures. RESIZE — модифицированная заявка
  // принимается. ACCEPT — заявка проходит как есть.
  if (risk_resp.decision() == fob::risk::v1::RISK_DECISION_HALT ||
      risk_resp.decision() == fob::risk::v1::RISK_DECISION_REJECT) {
    resp.set_accepted(false);
    *resp.mutable_error() = risk_resp.error();
    return resp;
  }
  fob::orders::v1::FlowOrder approved_order = req.order();
  // RESIZE: risk service подложил уменьшенную (или иначе скорректированную)
  // заявку — используем её вместо original.
  if (risk_resp.decision() == fob::risk::v1::RISK_DECISION_RESIZE &&
      risk_resp.has_resized_order()) {
    approved_order = risk_resp.resized_order();
  }

  // 3) Reserve funds in Ledger
  // BUY: reserve quote = total_qty * price_high (worst-case).
  // SELL: reserve base = total_qty
  fob::ledger::v1::ReserveFundsRequest led_req;
  *led_req.mutable_meta() = req.meta();
  // reservation_id = order_id обеспечивает идемпотентность Reserve.
  led_req.set_reservation_id(approved_order.order_id()); // idempotency: order id
  led_req.set_user_id(approved_order.user_id());
  led_req.set_order_id(approved_order.order_id());

  if (approved_order.side() == fob::common::v1::SIDE_BUY) {
    // Покупатель платит quote (USDT) за base (BTC). Резервируем максимум,
    // который потенциально потратим: qty * worst_price = qty * price_high.
    led_req.set_currency(approved_order.instrument().quote());
    Decimal qty = Decimal::from_proto(approved_order.total_qty());
    Decimal price = Decimal::from_proto(approved_order.price_high());
    Decimal notional = Decimal::mul(qty, price);
    *led_req.mutable_amount() = notional.to_proto();
    led_req.set_reason(fob::ledger::v1::RESERVE_REASON_NEW_ORDER);
  } else if (approved_order.side() == fob::common::v1::SIDE_SELL) {
    // Продавец отдаёт base (BTC) за quote. Резервируем total_qty BTC.
    led_req.set_currency(approved_order.instrument().base());
    *led_req.mutable_amount() = approved_order.total_qty();
    led_req.set_reason(fob::ledger::v1::RESERVE_REASON_NEW_ORDER);
  } else {
    // SIDE_UNSPECIFIED — программная ошибка вызывающего, не пользовательская.
    resp.set_accepted(false);
    auto* e = resp.mutable_error();
    e->set_code("SIDE_UNSPECIFIED");
    e->set_message("Order side must be BUY or SELL");
    return resp;
  }

  auto led_resp = ledger_.ReserveFunds(led_req);
  if (!led_resp.success()) {
    // Недостаточно средств / лимит / kill-switch — пробрасываем error пользователю.
    resp.set_accepted(false);
    *resp.mutable_error() = led_resp.error();
    return resp;
  }

  // 4) Persist order in memory (MVP)
  // Создаём stored = approved + computed поля (status, remaining_qty, updated_at).
  fob::orders::v1::FlowOrder stored = approved_order;
  stored.set_status(fob::common::v1::ORDER_STATUS_NEW);
  *stored.mutable_remaining_qty() = stored.total_qty();
  *stored.mutable_updated_at() = cex::common::now_ts();

  // Lock-scope блок: минимизируем удержание mutex'а.
  // RAII lock_guard разблокирует по выходу из {} даже при exception.
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
      // Drop the in-memory entry as well — иначе в Profile висит "fake-accepted"
      // заявка, которой нет в PG → matching её не увидит, но UI покажет.
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
  // (legacy path — даже когда PG включён, дублируем в Kafka для совместимости
  // с in-memory matching и для observability/replay).
  fob::orders::v1::OrdersNormalized evt;
  auto* meta = evt.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("order_flow");
  meta->set_correlation_id(req.meta().correlation_id());
  // partition_key = symbol гарантирует, что все события по одной паре идут
  // в одну partition → matching видит их в правильном порядке.
  meta->set_partition_key(stored.instrument().symbol()); // partition by symbol for matching locality

  auto* create = evt.mutable_create();
  *create->mutable_order() = stored;

  publisher_.publish(evt);

  resp.set_accepted(true);
  resp.set_order_id(stored.order_id());
  return resp;
}

// ============================================================================
// CancelFlowOrder — F-03.
//
// Шаги:
//   1. Lookup в in-memory orders_. NOT_FOUND если нет.
//   2. Set status=CANCELED + updated_at.
//   3. Publish cancel event в Kafka.
//   4. Ledger ReleaseFunds по reservation_id = order_id.
// ============================================================================
fob::orders::v1::CancelFlowOrderResponse OrderFlowUseCases::CancelFlowOrder(
    const fob::orders::v1::CancelFlowOrderRequest& req) {
  fob::orders::v1::CancelFlowOrderResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("order_flow");

  std::string symbol_for_partition;
  std::string user_id_for_cancel;
  {
    // Lock-scope: только in-memory update под mutex'ом.
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
    // Сохраним эти поля для использования ВНЕ lock'а.
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
  // Fallback на user_id_for_cancel если запрос не указал — старая API форма.
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

// ----------------------------------------------------------------------------
// GetFlowOrder — простой lookup. NOT_FOUND embed в view.error чтобы клиент
// получил структурированный response (а не gRPC error).
// ----------------------------------------------------------------------------
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

// ----------------------------------------------------------------------------
// ListFlowOrders — список заявок, опционально фильтр по user_id.
// Pattern: snapshot under lock → sort/serialize out of lock.
// Это критично для performance — не блокируем CreateFlowOrder во время
// sort/serialize большой коллекции.
// ----------------------------------------------------------------------------
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
      (void)order_id;   // unused-binding silencer
      // Filter по user_id если задан — UI просит "только мои заявки".
      if (!req.user_id().empty() && order.user_id() != req.user_id()) {
        continue;
      }
      snapshot.push_back(order);
    }
  }

  // Sort by created_at DESC — самые свежие сверху (UX convention).
  // Лямбда захватывает функцию timestamp_to_unix_ms по ссылке (free function).
  std::sort(snapshot.begin(), snapshot.end(),
            [](const fob::orders::v1::FlowOrder& left,
               const fob::orders::v1::FlowOrder& right) {
              return timestamp_to_unix_ms(left.created_at()) >
                     timestamp_to_unix_ms(right.created_at());
            });

  // std::move для каждого order в snapshot — uses move-assignment proto,
  // избегаем лишних копий.
  for (auto& order : snapshot) {
    *resp.add_views()->mutable_order() = std::move(order);
  }
  return resp;
}

// ============================================================================
// ApplyBatchResult — вызывается consumer'ом batch.outputs.
//
// Применяет fills как декременты remaining_qty + обрабатывает order_updates.
// Идемпотентность через applied_fills_ map (key = batch_id + "|" + order_id).
//
// Использование: на каждый BatchResult из Kafka вызывается один раз. Если
// сервис рестартует и consumer перечитывает offset → applied_fills_
// чистый, но это OK потому что мы хранится остаточное состояние в orders_
// (which сериализуется при future PG-persist phase).
// ============================================================================
void OrderFlowUseCases::ApplyBatchResult(
    const fob::matching::v1::BatchResult& batch) {
  std::lock_guard<std::mutex> lock(orders_mu_);

  int applied = 0;
  int duplicates = 0;
  int unknown = 0;

  // 1) Apply per-order fills as decrements to remaining_qty.
  for (const auto& fill : batch.fills()) {
    // Composite key обеспечивает idempotency на уровне (batch, order):
    // один fill за один batch на одну заявку — иначе double-decrement при
    // replay batch'а.
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
    // Терминальный статус: не реактивируем заявку даже если пришёл fill.
    // Это защита от: пользователь cancel → matching ещё успел дать partial fill
    // → мы должны игнорировать fill потому что заявка уже отменена.
    // Но idempotency-маркер ставим, чтобы при replay не считать ещё раз.
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
    // Hard floor: остаток не может быть отрицательным. CapFillsAgainstRemaining
    // в run_batch_uc должен был это предотвратить, но double-defence не лишнее.
    if (new_remaining.units < 0) {
      new_remaining.units = 0;  // hard floor — see BUG-2 (overfill defence)
    }
    *order.mutable_remaining_qty() = new_remaining.to_proto();

    // Status: FILLED если remaining=0, иначе PARTIALLY_FILLED.
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
    // Защита user's CANCEL: matching не может перетереть отмену
    // (race: cancel в момент когда matching уже подготовил update).
    if (order.status() == fob::common::v1::ORDER_STATUS_CANCELED) continue;
    if (update.has_remaining_qty()) {
      *order.mutable_remaining_qty() = update.remaining_qty();
    }
    if (update.status() != fob::common::v1::ORDER_STATUS_UNSPECIFIED) {
      order.set_status(update.status());
    }
    *order.mutable_updated_at() = cex::common::now_ts();
  }

  // Один summary-log на batch — не плодим line-noise при обычной работе.
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
