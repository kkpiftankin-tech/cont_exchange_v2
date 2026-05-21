#pragma once

#include <functional>
#include <string>

#include "cex/common/kafka.hpp"
#include "cex/common/replay_kafka.hpp"

namespace cex::gateway::infra {

class ReplayCommandsKafkaPublisher {
 public:
  using SendFn = std::function<bool(const std::string& topic,
                                    const std::string& key,
                                    const std::string& payload)>;

  explicit ReplayCommandsKafkaPublisher(cex::common::KafkaProducer producer,
                                        std::string topic = "replay.commands");
  explicit ReplayCommandsKafkaPublisher(SendFn send,
                                        std::string topic = "replay.commands");

  bool Publish(const cex::common::ReplayCommandMessage& msg);

 private:
  SendFn send_;
  std::string topic_;
};

}  // namespace cex::gateway::infra
