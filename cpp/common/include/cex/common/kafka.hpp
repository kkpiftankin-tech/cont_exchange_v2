#pragma once
// ============================================================================
// kafka.hpp — общая обёртка над librdkafka для producer/consumer.
//
// Назначение:
//   Минимальный shared API, чтобы каждый сервис не дублировал boilerplate
//   librdkafka конфигурации. Все Kafka-flow проекта (Producer / Consumer)
//   проходят через эти классы.
//
// Конвенции (CLAUDE.md §13):
//   - At-least-once + idempotent consumer (enable_auto_commit=false +
//     manual commitSync после успешной обработки).
//   - Key — meaningful: order_id / symbol / batch_id / intent_id.
//   - Replay possible: auto_offset_reset="earliest" по умолчанию для
//     audit/backtest сценариев.
//
// Производитель:
//   produce(topic, key, payload) — синхронный с точки зрения API, но
//   librdkafka внутренне buffer'ит. Для flush/sync — вызвать flush() на
//   shutdown (см. деструктор).
//
// Потребитель:
//   poll_once(timeout, handler) — обрабатывает один message-batch.
//   Handler вызывается с (topic, key, payload). Offset commit делается
//   автоматически после возврата handler'а (manual commitSync).
// ============================================================================
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <librdkafka/rdkafkacpp.h>

namespace cex::common {

/// Configuration для KafkaProducer. brokers — comma-separated host:port
/// list (одинаковый формат для kafka и redpanda).
struct KafkaConfig {
  std::string brokers;     // "redpanda:9092"
  std::string client_id;   // "order_flow"
};

/// Producer-обёртка. Один экземпляр на сервис обычно достаточен; librdkafka
/// поддерживает concurrent produce() из разных threads.
class KafkaProducer {
 public:
  explicit KafkaProducer(const KafkaConfig& cfg);

  // Send binary payload to topic with optional key (for partitioning).
  /// Key определяет partition: hash(key) % num_partitions. Передавайте
  /// смысловой key (order_id, symbol, batch_id) — это даёт partition-stability
  /// для idempotency и ordering.
  /// Возвращает false при transient error; librdkafka retries внутренне,
  /// но для guarantee использовать flush() на shutdown.
  bool produce(const std::string& topic,
               const std::string& key,
               const std::string& payload);

 private:
  std::unique_ptr<RdKafka::Producer> producer_;
};

/// Configuration для KafkaConsumer.
/// enable_auto_commit=false — обязательно для at-least-once (CLAUDE.md §13).
/// auto_offset_reset="earliest" — для replay/audit; "latest" если хотите
/// игнорировать прошлые messages при cold-start.
struct KafkaConsumerConfig {
  std::string brokers;
  std::string group_id;
  std::string client_id;
  bool enable_auto_commit{false};
  std::string auto_offset_reset{"earliest"};
  int max_poll_interval_ms{300000};
};

/// Consumer-обёртка. group_id определяет consumer-group (rebalance).
/// Один сервис обычно имеет один group_id; реплики сервиса делят
/// partitions между собой.
class KafkaConsumer {
 public:
  explicit KafkaConsumer(const KafkaConsumerConfig& cfg);

  /// subscribe — подписка на список топиков. Принимает несколько при
  /// необходимости (например, ledger consumer слушает batch.outputs +
  /// execution.venue).
  bool subscribe(const std::vector<std::string>& topics);

  // Poll one message and call handler(topic, key, payload).
  // Returns false if fatal error occurred.
  /// Single-poll mode: caller вызывает в while-loop. timeout_ms = max ожидание
  /// одного poll'а. Если в течение timeout нет messages — handler не вызывается,
  /// возвращается true.
  /// При manual commit (enable_auto_commit=false) commitSync вызывается
  /// автоматически после возврата handler'а — это гарантирует at-least-once.
  bool poll_once(int timeout_ms,
                 const std::function<void(const std::string& topic,
                                          const std::string& key,
                                          const std::string& payload)>& handler);

 private:
  std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
  bool manual_commit_{true};
};

}  // namespace cex::common
