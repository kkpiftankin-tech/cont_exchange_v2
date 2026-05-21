#pragma once

#include <thread>

#include "cex/common/kafka.hpp"
#include "app/risk_uc.hpp"

namespace cex::risk::infra {

class KafkaConsumer {
 public:
  KafkaConsumer(std::string brokers, app::RiskUseCases& uc);

  void start();
  void stop();

 private:
  void venue_liquidity_fob_loop();
  void venue_health_loop();
  void execution_reports_loop();
  void synthetic_orders_loop();

  std::atomic<bool> running_;
  std::string brokers_;
  app::RiskUseCases& uc_;

  std::thread venue_liquidity_fob_t_;
  std::thread venue_health_t_;
  std::thread execution_reports_t_;
  std::thread synthetic_orders_t_;

  cex::common::KafkaConsumer venue_liquidity_fob_consumer_;
  cex::common::KafkaConsumer venue_health_consumer_;
  cex::common::KafkaConsumer execution_reports_consumer_;
  cex::common::KafkaConsumer synthetic_orders_consumer_;
};

}  // namespace cex::risk::infra
