#pragma once
// =============================================================================
// VenuesLoop — MVP-симулятор внешних торговых площадок.
//
// Ответственности (в архитектуре):
//   * Адаптеры к внешним venue (Binance/Coinbase/...). Сейчас это симулятор.
//   * Публикация marketdata.raw — данные поступают в market_data сервис.
//   * Чтение execution.intents и публикация execution.reports — это поток
//     хедж-операций (matching попросил исполнить — venues исполнили).
//
// В MVP intents считаются мгновенно "filled" по limit_price (или по 100.00,
// если limit не указан). Этого достаточно для проверки сквозного флоу.
// =============================================================================

#include <atomic>
#include <thread>

#include "cex/common/kafka.hpp"

namespace cex::venues::app {

class VenuesLoop {
 public:
  explicit VenuesLoop(const std::string& brokers);

  // Поднимает оба фоновых потока. Не блокирующий вызов.
  void start();
  // Останов и join обоих потоков (для тестов / graceful shutdown).
  void stop();

 private:
  // Поток-1: периодически публикует синтетические тикеры в marketdata.raw.
  void md_publish_loop();
  // Поток-2: читает execution.intents и сразу публикует execution.reports.
  void exec_consume_loop();

  std::string brokers_;
  cex::common::KafkaProducer producer_;  // публикует и MD, и репорты
  cex::common::KafkaConsumer consumer_;  // читает execution.intents

  std::atomic<bool> running_{false};
  std::thread t_md_;
  std::thread t_exec_;
};

}  // namespace cex::venues::app
