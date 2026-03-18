#pragma once
#include <atomic>
#include <thread>

#include "cex/common/kafka.hpp"

namespace cex::observability::app {

// Observability service:
// - subscribes to important event topics and prints summaries
// - in production: send to OpenTelemetry/Prometheus/ClickHouse etc.
class ObsLoop {
 public:
  explicit ObsLoop(const std::string& brokers);

  void start();
  void stop();

 private:
  void loop();

  std::string brokers_;
  cex::common::KafkaConsumer consumer_;

  std::atomic<bool> running_{false};
  std::thread t_;
};

}  // namespace cex::observability::app
