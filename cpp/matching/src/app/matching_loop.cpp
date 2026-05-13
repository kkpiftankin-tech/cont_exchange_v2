// =============================================================================
// Реализация MatchingLoop. Содержит как application-слой (потоки, события),
// так и (временно, в MVP) "наивный" солвер внутри run_one_batch.
// =============================================================================

#include "app/matching_loop.hpp"

#include <chrono>

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"
#include "fob/matching/v1/batch.pb.h"

namespace cex::matching::app {

using cex::common::Decimal;

// Среднее арифметическое двух Decimal: (a + b) / 2.
// Делим целочисленно, после выравнивания scale в Decimal::add — для MVP ок.
static Decimal midpoint(const Decimal& a, const Decimal& b) {
  Decimal sum = Decimal::add(a, b);
  sum.units /= 2;
  return sum;
}

// Умножение скорости (значение/секунду) на длительность в миллисекундах.
// Реализация: q = per_sec * ms / 1000. Используем __int128, чтобы дать
// больший запас по переполнению при крупных значениях units.
static Decimal mul_by_ms(const Decimal& per_sec, int ms) {
  Decimal out = per_sec;
  __int128 u = static_cast<__int128>(out.units) * static_cast<__int128>(ms);
  u /= 1000;
  out.units = static_cast<int64_t>(u);
  return out;
}

// Конструктор: создаёт продюсера/консьюмера сразу же. Подписку и потоки
// разворачиваем в start(), чтобы конструктор не делал I/O.
MatchingLoop::MatchingLoop(const std::string& brokers, int batch_interval_ms)
    : brokers_(brokers),
      batch_interval_ms_(batch_interval_ms),
      producer_({.brokers=brokers, .client_id="matching"}),
      // enable_auto_commit=false — коммитим вручную после успешной обработки.
      consumer_({.brokers=brokers, .group_id="matching", .client_id="matching", .enable_auto_commit=false}) {}

void MatchingLoop::start() {
  running_.store(true);
  // Подписка одна-единственная: вход для матчинга.
  consumer_.subscribe({"orders.normalized"});
  // Поднимаем фоновые потоки: один потребляет, другой считает батч.
  t_consume_ = std::thread([this] { consume_orders_loop(); });
  t_batch_   = std::thread([this] { batch_timer_loop(); });
}

void MatchingLoop::stop() {
  running_.store(false);
  if (t_consume_.joinable()) t_consume_.join();
  if (t_batch_.joinable())   t_batch_.join();
}

// Поток потребления: блокируется в poll_once на 500 мс, потом повторяет.
// При фатальной ошибке Kafka выходит из цикла — внешний k8s/docker рестартует
// процесс, новый консьюмер заберёт оффсеты с того же места (т.к. коммит ручной).
void MatchingLoop::consume_orders_loop() {
  while (running_.load()) {
    bool ok = consumer_.poll_once(500, [this](const std::string& topic,
                                            const std::string& key,
                                            const std::string& payload) {
      (void)topic; (void)key; // сейчас не нужны — топик один, key мы не используем
      fob::orders::v1::OrdersNormalized evt;
      if (!cex::common::from_bytes(payload, evt)) {
        cex::common::log_json("ERROR", "Failed to parse OrdersNormalized");
        return;
      }
      on_order_event(evt);
    });
    if (!ok) break;
  }
}

// Поток таймера: ждёт интервал и зовёт solver. Намеренно простой sleep_for —
// для MVP допустимо. В проде стоит делать "next_tick"-схему, чтобы фактическая
// длительность работы не накапливала задержку (drift).
void MatchingLoop::batch_timer_loop() {
  using namespace std::chrono;
  while (running_.load()) {
    std::this_thread::sleep_for(milliseconds(batch_interval_ms_));
    run_one_batch();
  }
}

// Обработка событий (create/cancel/amend) — мутации реестра active_.
void MatchingLoop::on_order_event(const fob::orders::v1::OrdersNormalized& evt) {
  if (evt.has_create()) {
    // Новый ордер: добавляем в реестр. Если уже был — перезаписываем (идемпотентно).
    const auto& o = evt.create().order();
    active_[o.order_id()] = o;
    cex::common::log_json("INFO", "Order added to matching",
                          {{"order_id", o.order_id()},
                           {"symbol", o.instrument().symbol()}});
  } else if (evt.has_cancel()) {
    // Отмена: убираем из реестра. erase по отсутствующему ключу — no-op.
    const auto& c = evt.cancel();
    active_.erase(c.order_id());
    cex::common::log_json("INFO", "Order canceled in matching",
                          {{"order_id", c.order_id()}});
  } else if (evt.has_amend()) {
    // Изменение параметров. В MVP мутируем только те поля, которые присутствуют
    // в сообщении (proto3 has_<field>() работает для message-типов).
    const auto& a = evt.amend();
    auto it = active_.find(a.order_id());
    if (it != active_.end()) {
      if (a.has_new_total_qty())  *it->second.mutable_total_qty()  = a.new_total_qty();
      if (a.has_new_price_low())  *it->second.mutable_price_low()  = a.new_price_low();
      if (a.has_new_price_high()) *it->second.mutable_price_high() = a.new_price_high();
      if (a.has_new_max_speed())  *it->second.mutable_max_speed()  = a.new_max_speed();
      cex::common::log_json("INFO", "Order amended in matching",
                            {{"order_id", a.order_id()}});
    }
  }
}

// Главный шаг матчинга. ВАЖНО: это НЕ настоящий многомерный континуальный
// солвер; это упрощение, которое позволяет проверить хореографию сервисов
// и учёт в ledger в dev-окружении. Реальный солвер будет жить за интерфейсом
// IContinuousClearingSolver (см. domain/solver.hpp).
void MatchingLoop::run_one_batch() {
  if (active_.empty()) return;

  // Готовим заголовок батча: id события, источник, корреляционный id и т.п.
  fob::matching::v1::BatchResult batch;
  auto* meta = batch.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("matching");
  meta->set_correlation_id(cex::common::uuid_v4());
  meta->set_partition_key("batch"); // в dev все батчи в одной партиции

  batch.set_batch_id(cex::common::uuid_v4());
  *batch.mutable_timestamp() = cex::common::now_ts();

  // Диагностика для observability/dashboards.
  auto* diag = batch.mutable_diagnostics();
  diag->set_residual_norm(0.0);
  diag->set_solve_time_ms(1);
  diag->set_num_active_orders(static_cast<uint32_t>(active_.size()));
  diag->set_solver_diagnostics_json("{\"kind\":\"mvp_simulator\"}");
  diag->set_config_version(1);

  // После пробега удалим полностью исполненные ордера.
  std::vector<std::string> to_erase;

  // Накопитель для расчёта средней клиринговой цены по символу:
  //   { symbol -> (sum_of_midpoints, count) }.
  std::unordered_map<std::string, std::pair<Decimal,int>> sym_sum;

  // Простой проход по всем активным ордерам.
  for (auto& [oid, o] : active_) {
    Decimal rem   = Decimal::from_proto(o.remaining_qty());
    Decimal speed = Decimal::from_proto(o.max_speed());

    // Сколько можно "налить" за один батч-интервал.
    Decimal dq = mul_by_ms(speed, batch_interval_ms_);
    if (Decimal::cmp(dq, rem) > 0) dq = rem; // не больше остатка

    if (dq.units <= 0) continue; // нечего матчить

    // Цена исполнения — середина диапазона [price_low, price_high].
    // Для реального солвера здесь будет результат оптимизации.
    Decimal p = midpoint(Decimal::from_proto(o.price_low()),
                         Decimal::from_proto(o.price_high()));

    // Стоимость исполненной части = dq * p.
    Decimal notional = Decimal::mul(dq, p);

    // Запись о fill'е (исполнении) — её увидит ledger и обновит балансы.
    auto* f = batch.add_fills();
    f->set_order_id(o.order_id());
    f->set_user_id(o.user_id());
    *f->mutable_instrument() = o.instrument();
    f->set_side(o.side());
    *f->mutable_executed_qty()     = dq.to_proto();
    *f->mutable_price()            = p.to_proto();
    *f->mutable_executed_notional() = notional.to_proto();

    // executed_rates — фактически "скорость" (qty/sec) исполнения.
    // В нашем простом случае dq = speed * dt, поэтому скорость == speed.
    (*batch.mutable_executed_rates())[o.order_id()] = speed.to_proto();

    // Уменьшаем остаток и фиксируем время апдейта.
    rem = Decimal::sub(rem, dq);
    *o.mutable_remaining_qty() = rem.to_proto();
    *o.mutable_updated_at() = cex::common::now_ts();

    // Дельта-апдейт по ордеру: нужно сервисам, которые держат свой кэш статусов.
    auto* upd = batch.add_order_updates();
    upd->set_order_id(o.order_id());
    upd->set_status(rem.units == 0 ? fob::common::v1::ORDER_STATUS_FILLED
                                   : fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED);
    *upd->mutable_remaining_qty()    = rem.to_proto();
    *upd->mutable_filled_qty_total() = Decimal::sub(Decimal::from_proto(o.total_qty()), rem).to_proto();
    *upd->mutable_updated_at()       = cex::common::now_ts();

    if (rem.units == 0) to_erase.push_back(oid);

    // Аккумулируем цены для расчёта средней по символу.
    auto& acc = sym_sum[o.instrument().symbol()];
    if (acc.second == 0) acc.first = p; else acc.first = Decimal::add(acc.first, p);
    acc.second += 1;
  }

  // Средняя клиринговая цена по символу — для market_data и публичного фида.
  for (auto& [sym, pair] : sym_sum) {
    Decimal avg = pair.first;
    avg.units /= pair.second;
    (*batch.mutable_clear_prices())[sym] = avg.to_proto();
  }

  // Удаляем полностью исполненные ордера из реестра.
  for (auto& oid : to_erase) active_.erase(oid);

  // Публикация результата в Kafka.
  // Партиционируем по batch_id — для текущего MVP неважно, но удобно для
  // последующих consumers, которым нужно видеть один batch как единицу.
  const std::string topic = "batch.outputs";
  const std::string key = batch.batch_id();
  producer_.produce(topic, key, cex::common::to_bytes(batch));

  cex::common::log_json("INFO", "Produced batch.outputs",
                        {{"batch_id", batch.batch_id()},
                         {"fills", std::to_string(batch.fills_size())},
                         {"active_after", std::to_string(active_.size())}});
}

}  // namespace cex::matching::app
