#pragma once

// ============================================================================
// positions_hub.hpp — thread-safe fan-out для positions.update сигналов (F6-5).
//
// Назначение (ADR-046):
//   ledger публикует лёгкий invalidation-сигнал в топик positions.update
//   ({user_id, batch_id, ts}). Kafka-consumer парсит его и вызывает
//   Publish(user_id, batch_id). Hub раздаёт событие всем WS-listener'ам;
//   listener сам реагрегирует свежий снимок через BuildPositionsJson(user_id).
//
//   Сигнал НЕ несёт состояния (нет позиций/маржи) — это команда «перечитай».
//   Поэтому hub хранит только (user_id, batch_id), а не сам снимок.
//
// По образцу ReplayResultsHub (replay_results_hub.hpp): keyed by user_id,
// Subscribe/Publish/Unsubscribe, изоляция исключений listener'ов, чтобы
// сломанный WS не отравил доставку остальным подписчикам.
// ============================================================================

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cex::gateway::infra {

class PositionsHub {
 public:
  struct Event {
    std::string user_id;
    std::string batch_id;
  };
  using Listener = std::function<void(const Event&)>;

  // Раздаёт сигнал «позиции user_id изменились батчем batch_id» всем
  // подписанным listener'ам. Исключения listener'ов изолированы.
  void Publish(const std::string& user_id, const std::string& batch_id);

  // Подписка WS-listener'а. Возвращает id для последующего Unsubscribe.
  uint64_t Subscribe(Listener listener);
  void Unsubscribe(uint64_t listener_id);

 private:
  mutable std::mutex mu_;
  uint64_t next_listener_id_{1};
  std::unordered_map<uint64_t, Listener> listeners_;
};

}  // namespace cex::gateway::infra
