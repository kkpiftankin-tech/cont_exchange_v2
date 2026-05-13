// =============================================================================
// Реализация обёрток над rdkafka++ — KafkaProducer и KafkaConsumer.
// Все диагностические сообщения уходят в JSON-логгер (cex::common::log_json),
// чтобы единообразно собирать ошибки шины во всех сервисах.
// =============================================================================

#include "cex/common/kafka.hpp"
#include "cex/common/log.hpp"

namespace cex::common {

// --- Продюсер ----------------------------------------------------------------

KafkaProducer::KafkaProducer(const KafkaConfig& cfg) {
  std::string errstr;

  // Глобальная конфигурация: задаём только обязательные параметры.
  // Остальное librdkafka берёт из дефолтов (acks=1, batch.* и т.д.).
  RdKafka::Conf* conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
  conf->set("bootstrap.servers", cfg.brokers, errstr);
  conf->set("client.id", cfg.client_id, errstr);

  // Создаём производителя; сразу удаляем conf — копию он держит сам.
  producer_.reset(RdKafka::Producer::create(conf, errstr));
  delete conf;

  if (!producer_) {
    log_json("ERROR", "KafkaProducer create failed", {{"err", errstr}});
  } else {
    log_json("INFO", "KafkaProducer created", {{"brokers", cfg.brokers}, {"client_id", cfg.client_id}});
  }
}

bool KafkaProducer::produce(const std::string& topic,
                            const std::string& key,
                            const std::string& payload) {
  // Если конструктор завершился неудачно — никуда не пишем, чтобы не падать.
  if (!producer_) return false;

  // PARTITION_UA = "unassigned" — librdkafka сам выберет партицию по ключу.
  // RK_MSG_COPY — копируем payload в очередь, после возврата буфер можно освобождать.
  RdKafka::ErrorCode ec = producer_->produce(
      topic,
      RdKafka::Topic::PARTITION_UA,
      RdKafka::Producer::RK_MSG_COPY,
      const_cast<char*>(payload.data()),
      payload.size(),
      key.empty() ? nullptr : key.data(),
      key.empty() ? 0 : key.size(),
      0,        // timestamp = брокер проставит сам
      nullptr); // msg_opaque не нужен

  if (ec != RdKafka::ERR_NO_ERROR) {
    log_json("ERROR", "Kafka produce failed", {{"topic", topic}, {"err", RdKafka::err2str(ec)}});
    return false;
  }

  // poll(0) — обязательный вызов: даёт librdkafka возможность обработать
  // delivery callbacks и не накапливать память бесконечно.
  producer_->poll(0);
  return true;
}

// --- Консьюмер ---------------------------------------------------------------

KafkaConsumer::KafkaConsumer(const KafkaConsumerConfig& cfg) {
  std::string errstr;
  RdKafka::Conf* conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);

  conf->set("bootstrap.servers", cfg.brokers, errstr);
  conf->set("group.id", cfg.group_id, errstr);
  conf->set("client.id", cfg.client_id, errstr);
  // Авто-коммит обычно отключаем: коммитим вручную ПОСЛЕ успешной обработки,
  // иначе при крэше можно потерять сообщение между коммитом и обработкой.
  conf->set("enable.auto.commit", cfg.enable_auto_commit ? "true" : "false", errstr);
  // earliest: для нового consumer group читаем с самого начала топика.
  // Это нужно при первом старте сервиса в dev-стенде.
  conf->set("auto.offset.reset", "earliest", errstr);

  consumer_.reset(RdKafka::KafkaConsumer::create(conf, errstr));
  delete conf;

  if (!consumer_) {
    log_json("ERROR", "KafkaConsumer create failed", {{"err", errstr}});
  } else {
    log_json("INFO", "KafkaConsumer created", {{"brokers", cfg.brokers}, {"group_id", cfg.group_id}});
  }
}

bool KafkaConsumer::subscribe(const std::vector<std::string>& topics) {
  if (!consumer_) return false;
  RdKafka::ErrorCode ec = consumer_->subscribe(topics);
  if (ec != RdKafka::ERR_NO_ERROR) {
    log_json("ERROR", "Kafka subscribe failed", {{"err", RdKafka::err2str(ec)}});
    return false;
  }
  return true;
}

bool KafkaConsumer::poll_once(
    int timeout_ms,
    const std::function<void(const std::string& topic,
                             const std::string& key,
                             const std::string& payload)>& handler) {
  if (!consumer_) return false;

  // unique_ptr с RAII-удалением — сообщение освобождается на выходе из функции.
  std::unique_ptr<RdKafka::Message> msg(consumer_->consume(timeout_ms));
  if (!msg) return true;

  // Тайм-аут polling'а — норма, просто ничего не пришло.
  if (msg->err() == RdKafka::ERR__TIMED_OUT) return true;
  // Достигли конца партиции — тоже норма, ждём следующих сообщений.
  if (msg->err() == RdKafka::ERR__PARTITION_EOF) return true;

  // Любая другая ошибка — фатально для текущего цикла, возвращаем false.
  if (msg->err() != RdKafka::ERR_NO_ERROR) {
    log_json("ERROR", "Kafka consume error", {{"err", msg->errstr()}});
    return false;
  }

  // Извлекаем поля сообщения и передаём пользовательскому хендлеру.
  std::string topic = msg->topic_name();
  std::string key;
  if (msg->key()) key = *msg->key();
  std::string payload(static_cast<const char*>(msg->payload()), msg->len());

  handler(topic, key, payload);

  // Ручной синхронный коммит смещения после успешной обработки.
  // При at-least-once это гарантирует, что недообработанные сообщения
  // будут перепрочитаны после рестарта сервиса.
  consumer_->commitSync(msg.get());
  return true;
}

} // namespace cex::common
