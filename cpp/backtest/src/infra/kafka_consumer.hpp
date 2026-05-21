#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "cex/common/kafka.hpp"
#include "app/backtest_uc.hpp"

namespace cex::backtest::infra {

class BacktestKafkaConsumer {
 public:
  BacktestKafkaConsumer(app::BacktestUseCases* uc,
                        const std::string& brokers);

  void start();
  void stop();

 private:
  void loop();

  app::BacktestUseCases* uc_;
  std::string brokers_;
  std::atomic<bool> running_{false};
  std::thread t_;
};

}  // namespace cex::backtest::infra
