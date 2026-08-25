#pragma once
// ============================================================================
// positions_update_publisher.hpp — producer Kafka-топика positions.update.
//
// Назначение (F-06 / F6-5, ADR-046, T-F06-072):
//   После успешного ApplyBatchResult ledger публикует лёгкий
//   invalidation-сигнал «позиции пользователя изменились» в топик
//   positions.update. gateway (ws-gateway) консьюмит сигнал, реагрегирует
//   полный снимок позиций/маржи и пушит его подписанному клиенту по WS.
//
// Контракт (docs/06-api/messaging/positions.update.md):
//   key     = user_id        — push маршрутизируется/партиционируется по юзеру.
//   payload = JSON {user_id, batch_id, ts}  — сигнал НЕ несёт состояния.
//
// Delivery: at-least-once (ADR-020). Потребитель идемпотентен по batch_id.
// Публикация best-effort: ошибка produce логируется, но НЕ валит обработку
// батча (CLAUDE.md §17 — ledger source of truth, push — вторичный сигнал).
// ============================================================================
#include <cstdint>
#include <string>

#include "app/persistence_ports.hpp"
#include "cex/common/kafka.hpp"

namespace cex::ledger::infra {

/// Concrete producer поверх cex::common::KafkaProducer.
/// Реализует app-слойный port PositionsUpdatePublisherPort (для DI/тестов).
/// Топик — positions.update, key = user_id, payload — JSON.
class PositionsUpdatePublisher : public cex::ledger::app::PositionsUpdatePublisherPort {
 public:
  explicit PositionsUpdatePublisher(cex::common::KafkaProducer producer);

  bool Publish(const std::string& user_id,
               const std::string& batch_id,
               int64_t ts_unix) override;

 private:
  cex::common::KafkaProducer producer_;
};

}  // namespace cex::ledger::infra
