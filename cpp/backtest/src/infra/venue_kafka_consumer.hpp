#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "cex/common/kafka.hpp"
#include "app/venue_replay_uc.hpp"

namespace cex::backtest::infra {

class VenueKafkaConsumer {
 public:
  VenueKafkaConsumer(app::VenueReplayUseCases* uc,
                     const std::string& brokers);

  void start();
  void stop();

 private:
  void loop();

  app::VenueReplayUseCases* uc_;
  std::string brokers_;
  std::atomic<bool> running_{false};
  std::thread t_;
};

}  // namespace cex::backtest::infra
