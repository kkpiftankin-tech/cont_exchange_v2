#include "infra/replay_commands_kafka_publisher.hpp"

#include <memory>
#include <utility>

#include "cex/common/log.hpp"

namespace cex::gateway::infra {

ReplayCommandsKafkaPublisher::ReplayCommandsKafkaPublisher(cex::common::KafkaProducer producer,
                                                           std::string topic)
    : topic_(std::move(topic)) {
  auto shared_producer =
      std::make_shared<cex::common::KafkaProducer>(std::move(producer));
  send_ = [shared_producer](const std::string& topic_name,
                            const std::string& key,
                            const std::string& payload) {
    return shared_producer->produce(topic_name, key, payload);
  };
}

ReplayCommandsKafkaPublisher::ReplayCommandsKafkaPublisher(SendFn send,
                                                           std::string topic)
    : send_(std::move(send)), topic_(std::move(topic)) {}

bool ReplayCommandsKafkaPublisher::Publish(const cex::common::ReplayCommandMessage& msg) {
  if (!send_) return false;
  const bool ok = send_(topic_, msg.session_id, cex::common::SerializeReplayCommandMessage(msg));
  if (ok) {
    cex::common::log_json("INFO", "Published replay command",
                          {{"topic", topic_},
                           {"session_id", msg.session_id},
                           {"command_type",
                            cex::common::ReplayCommandTypeToString(msg.command_type)}});
  }
  return ok;
}

}  // namespace cex::gateway::infra
