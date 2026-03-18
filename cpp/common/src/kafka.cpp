#include "cex/common/kafka.hpp"
#include "cex/common/log.hpp"

namespace cex::common {

KafkaProducer::KafkaProducer(const KafkaConfig& cfg) {
  std::string errstr;

  RdKafka::Conf* conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
  conf->set("bootstrap.servers", cfg.brokers, errstr);
  conf->set("client.id", cfg.client_id, errstr);

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
  if (!producer_) return false;

  RdKafka::ErrorCode ec = producer_->produce(
      topic,
      RdKafka::Topic::PARTITION_UA,
      RdKafka::Producer::RK_MSG_COPY,
      const_cast<char*>(payload.data()),
      payload.size(),
      key.empty() ? nullptr : key.data(),
      key.empty() ? 0 : key.size(),
      0,
      nullptr);

  if (ec != RdKafka::ERR_NO_ERROR) {
    log_json("ERROR", "Kafka produce failed", {{"topic", topic}, {"err", RdKafka::err2str(ec)}});
    return false;
  }

  producer_->poll(0);
  return true;
}

KafkaConsumer::KafkaConsumer(const KafkaConsumerConfig& cfg) {
  std::string errstr;
  RdKafka::Conf* conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);

  conf->set("bootstrap.servers", cfg.brokers, errstr);
  conf->set("group.id", cfg.group_id, errstr);
  conf->set("client.id", cfg.client_id, errstr);
  conf->set("enable.auto.commit", cfg.enable_auto_commit ? "true" : "false", errstr);
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

  std::unique_ptr<RdKafka::Message> msg(consumer_->consume(timeout_ms));
  if (!msg) return true;

  if (msg->err() == RdKafka::ERR__TIMED_OUT) return true;
  if (msg->err() == RdKafka::ERR__PARTITION_EOF) return true;

  if (msg->err() != RdKafka::ERR_NO_ERROR) {
    log_json("ERROR", "Kafka consume error", {{"err", msg->errstr()}});
    return false;
  }

  std::string topic = msg->topic_name();
  std::string key;
  if (msg->key()) key = *msg->key();
  std::string payload(static_cast<const char*>(msg->payload()), msg->len());

  handler(topic, key, payload);

  // Manual commit (for MVP: commit after processing)
  consumer_->commitSync(msg.get());
  return true;
}

} // namespace cex::common
