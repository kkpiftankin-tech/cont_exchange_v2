#pragma once
#include <atomic>
#include <thread>

#include "cex/common/kafka.hpp"
#include "app/market_data_uc.hpp"

namespace cex::market_data::infra {

class MarketDataKafkaConsumer {
 public:
  MarketDataKafkaConsumer(app::MarketDataUseCases* uc,
                          const std::string& brokers);

  void start();
  void stop();

 private:
  void loop();

  app::MarketDataUseCases* uc_;
  std::string brokers_;
  std::atomic<bool> running_{false};
  std::thread t_;
};

}  // namespace cex::market_data::infra
