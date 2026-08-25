#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "cex/common/kafka.hpp"
#include "infra/replay_results_hub.hpp"

namespace cex::gateway::infra {

class ReplayResultsKafkaConsumer {
 public:
  ReplayResultsKafkaConsumer(ReplayResultsHub* hub,
                             std::string brokers,
                             std::string topic = "replay.results");

  void start();
  void stop();

 private:
  void loop();

  ReplayResultsHub* hub_;
  std::string brokers_;
  std::string topic_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace cex::gateway::infra
