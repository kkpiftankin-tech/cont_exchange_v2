#pragma once
#include <atomic>
#include <thread>

#include "cex/common/kafka.hpp"

namespace cex::venues::app {

// "Venues" service responsibilities in architecture:
// - adapters to external venues (CCXT in other language; here MVP simulator).
// - publish marketdata.raw
// - consume execution.intents and publish execution.reports
class VenuesLoop {
 public:
  explicit VenuesLoop(const std::string& brokers);

  void start();
  void stop();

 private:
  void md_publish_loop();
  void exec_consume_loop();

  std::string brokers_;
  cex::common::KafkaProducer producer_;
  cex::common::KafkaConsumer consumer_;

  std::atomic<bool> running_{false};
  std::thread t_md_;
  std::thread t_exec_;
};

}  // namespace cex::venues::app
