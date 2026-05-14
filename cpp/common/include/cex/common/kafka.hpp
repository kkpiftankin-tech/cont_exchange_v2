#pragma once
// =============================================================================
// Тонкие C++-обёртки над rdkafka++ (librdkafka), чтобы изолировать сервисы
// от деталей API и упростить тестирование/моки.
//
// Используются всеми сервисами, обменивающимися сообщениями через Kafka/Redpanda:
//   - order_flow публикует команды в orders.commands
//   - matching читает orders.commands и пишет matches.events
//   - ledger подписан на matches.events и применяет изменения балансов
//   - market_data считывает поток сделок и обновляет ОБ для клиентов
//   - и т.д.
// =============================================================================

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <librdkafka/rdkafkacpp.h>

namespace cex::common {

// Конфигурация продюсера. brokers — список адресов брокеров через запятую,
// client_id — идентификатор клиента, виден в логах брокера.
struct KafkaConfig {
  std::string brokers;     // например "redpanda:9092"
  std::string client_id;   // например "order_flow"
};

// Продюсер с минимальным API: один метод produce(...).
// Внутри держит unique_ptr на RdKafka::Producer, очищается автоматически.
class KafkaProducer {
 public:
  // Создание производителя. При ошибке — пишет ERROR в лог,
  // но не кидает исключение (для удобства запуска контейнеров).
  explicit KafkaProducer(const KafkaConfig& cfg);

  // Отправляет бинарный payload в указанный топик.
  // key используется как ключ партиционирования: одинаковые ключи попадают
  // в одну партицию, что критично для упорядоченности (например, по order_id).
  // Возвращает true если сообщение успешно поставлено в очередь отправки.
  bool produce(const std::string& topic,
               const std::string& key,
               const std::string& payload);

 private:
  std::unique_ptr<RdKafka::Producer> producer_;
};

// Конфигурация консьюмера.
struct KafkaConsumerConfig {
  std::string brokers;
  std::string group_id;             // consumer group; смещения коммитятся на группу
  std::string client_id;
  bool enable_auto_commit{false};   // мы ручной коммит делаем после успешной обработки
};

// Консьюмер: подписывается на топики и в poll_once() возвращает по одному сообщению.
class KafkaConsumer {
 public:
  explicit KafkaConsumer(const KafkaConsumerConfig& cfg);

  // Подписаться на список топиков. Вызывается один раз перед циклом poll'а.
  bool subscribe(const std::vector<std::string>& topics);

  // Опросить очередь на timeout_ms миллисекунд и при наличии сообщения вызвать handler.
  // handler получает имя топика, ключ и сырые байты payload — как пришло из брокера.
  // Возвращает false при фатальной ошибке (тогда зовущий должен прервать цикл/перезапуститься).
  // Тайм-ауты и EOF партиции — это норма, возвращаем true.
  bool poll_once(int timeout_ms,
                 const std::function<void(const std::string& topic,
                                          const std::string& key,
                                          const std::string& payload)>& handler);

 private:
  std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
};

}  // namespace cex::common
