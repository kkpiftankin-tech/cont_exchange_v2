#pragma once
// =============================================================================
// ObsLoop — фоновый поток, который слушает несколько важных топиков и пишет
// о них структурированные JSON-логи.
//
// В архитектуре методологии этот сервис должен превратиться в полноценный
// шлюз телеметрии (метрики/трейсы/алерты), пока что — минимальный проксик в логи.
// =============================================================================

#include <atomic>
#include <thread>

#include "cex/common/kafka.hpp"

namespace cex::observability::app {

class ObsLoop {
 public:
  explicit ObsLoop(const std::string& brokers);

  // Подписка и запуск потока. Не блокирующий.
  void start();
  // Останов потока (для тестов / graceful shutdown).
  void stop();

 private:
  void loop(); // тело фонового потока

  std::string brokers_;
  cex::common::KafkaConsumer consumer_; // один консьюмер на все топики

  std::atomic<bool> running_{false};
  std::thread t_;
};

}  // namespace cex::observability::app
