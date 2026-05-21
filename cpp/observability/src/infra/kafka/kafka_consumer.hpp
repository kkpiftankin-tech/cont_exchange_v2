#pragma once

#include <thread>

#include "app/service.hpp"

namespace cex::observability::infra {

class KafkaConsumer {
 public:
  KafkaConsumer(std::string brokers, app::Service& service);

  void start();
  void stop();

 private:
  void loop();

  std::string brokers_;
  app::Service& service_;
  std::atomic<bool> running_;
  std::thread t_;
};

}  // namespace cex::observability::infra
