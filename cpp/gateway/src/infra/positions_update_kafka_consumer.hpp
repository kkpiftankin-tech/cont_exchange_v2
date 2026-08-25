#pragma once

// ============================================================================
// positions_update_kafka_consumer.hpp — фоновый Kafka consumer для топика
// positions.update (F6-5, ADR-046).
//
// Назначение:
//   Поллит топик positions.update, парсит лёгкий сигнал {user_id, batch_id, ts}
//   и вызывает PositionsHub::Publish(user_id, batch_id). Hub раздаёт сигнал
//   WS-listener'ам, которые реагрегируют свежий снимок позиций.
//
//   Сигнал at-least-once и может продублироваться (ADR-020), поэтому consumer
//   идемпотентен по (user_id, batch_id): уже обработанный batch_id для того же
//   user_id повторно в hub не пушится.
//
// По образцу ReplayResultsKafkaConsumer (replay_results_kafka_consumer.hpp):
// отдельный поток, poll_once в while-loop, понятный lifecycle (start/stop).
// ============================================================================

#include <atomic>
#include <cstddef>
#include <deque>
#include <string>
#include <thread>
#include <unordered_set>

#include "infra/positions_hub.hpp"

namespace cex::gateway::infra {

class PositionsUpdateKafkaConsumer {
 public:
  PositionsUpdateKafkaConsumer(PositionsHub* hub,
                               std::string brokers,
                               std::string topic = "positions.update");

  void start();
  void stop();

  // Обрабатывает один payload (parse + dedup + hub->Publish). Выделено для
  // юнит-тестирования без живой Kafka. Возвращает true, если сигнал был
  // запушен в hub (false — невалидный payload либо дубликат batch_id).
  bool HandlePayload(const std::string& payload);

 private:
  void loop();

  PositionsHub* hub_;
  std::string brokers_;
  std::string topic_;
  std::atomic<bool> running_{false};
  std::thread thread_;

  // Дедуп по (user_id, batch_id). Ограниченное окно, чтобы память не росла
  // безгранично: храним последние kDedupCapacity ключей в FIFO-порядке.
  static constexpr std::size_t kDedupCapacity = 4096;
  std::unordered_set<std::string> seen_keys_;
  std::deque<std::string> seen_order_;
};

}  // namespace cex::gateway::infra
