#pragma once
// =============================================================================
// MatchingLoop — слой "application" в гексагональной архитектуре сервиса
// matching. Связывает воедино:
//   * входящий поток ордеров (consumer Kafka, топик orders.normalized);
//   * хранилище активных ордеров в памяти (active_);
//   * периодический батч-таймер, который запускает "решатель";
//   * исходящий поток результатов батча (producer Kafka, топик batch.outputs).
//
// Архитектурно реальный континуальный солвер должен быть отдельной
// абстракцией (см. domain/solver.hpp). Здесь, в MVP, он встроен в run_one_batch.
// =============================================================================

#include <atomic>
#include <thread>
#include <unordered_map>

#include "cex/common/kafka.hpp"
#include "cex/common/decimal.hpp"
#include "fob/orders/v1/orders.pb.h"

namespace cex::matching::app {

// Простой периодический матчинг (MVP-симулятор):
//   - потребляет orders.normalized (create / cancel / amend);
//   - раз в batch_interval_ms запускает "псевдо-солвер":
//        dq = min(remaining_qty, max_speed * dt);
//        цена исполнения = midpoint(price_low, price_high);
//   - публикует batch.outputs (fob.matching.v1.BatchResult).
class MatchingLoop {
 public:
  // brokers — список Kafka-брокеров; batch_interval_ms — период работы солвера.
  MatchingLoop(const std::string& brokers, int batch_interval_ms);

  // Запускает фоновые потоки потребления и таймера. Не блокирующий.
  void start();
  // Корректно останавливает фоновые потоки (для тестов и graceful-shutdown).
  void stop();

 private:
  // Поток-1: бесконечно poll'ит Kafka и парсит OrdersNormalized.
  void consume_orders_loop();
  // Поток-2: спит batch_interval_ms и зовёт run_one_batch().
  void batch_timer_loop();

  // Применить одно событие ордера (create/cancel/amend) к active_.
  void on_order_event(const fob::orders::v1::OrdersNormalized& evt);
  // Прогон одного шага матчинга по всем активным ордерам.
  void run_one_batch();

  std::string brokers_;
  int batch_interval_ms_;

  cex::common::KafkaProducer producer_; // публикует BatchResult
  cex::common::KafkaConsumer consumer_; // читает OrdersNormalized

  // Флаг работы. atomic — потому что читается из обоих фоновых потоков.
  std::atomic<bool> running_{false};
  std::thread t_consume_;
  std::thread t_batch_;

  // Активные ордера в памяти, ключ — order_id.
  // ВАЖНО: в продакшене состояние должно быть либо персистентным
  // (например, RocksDB снапшоты + восстановление из Kafka), либо
  // явно реплицируемым. Сейчас при рестарте сервиса всё теряется.
  std::unordered_map<std::string, fob::orders::v1::FlowOrder> active_;
};

}  // namespace cex::matching::app
