#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <librdkafka/rdkafkacpp.h>

namespace cex::common {

struct KafkaConfig {
  std::string brokers;     // "redpanda:9092"
  std::string client_id;   // "order_flow"
};

class KafkaProducer {
 public:
  explicit KafkaProducer(const KafkaConfig& cfg);

  // Send binary payload to topic with optional key (for partitioning).
  bool produce(const std::string& topic,
               const std::string& key,
               const std::string& payload);

 private:
  std::unique_ptr<RdKafka::Producer> producer_;
};

struct KafkaConsumerConfig {
  std::string brokers;
  std::string group_id;
  std::string client_id;
  bool enable_auto_commit{false};
  std::string auto_offset_reset{"earliest"};
  int max_poll_interval_ms{300000};
};

class KafkaConsumer {
 public:
  explicit KafkaConsumer(const KafkaConsumerConfig& cfg);

  bool subscribe(const std::vector<std::string>& topics);

  // Poll one message and call handler(topic, key, payload).
  // Returns false if fatal error occurred.
  bool poll_once(int timeout_ms,
                 const std::function<void(const std::string& topic,
                                          const std::string& key,
                                          const std::string& payload)>& handler);

 private:
  std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
  bool manual_commit_{true};
};

}  // namespace cex::common
