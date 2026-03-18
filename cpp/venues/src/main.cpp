#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/venues_loop.hpp"

#include <chrono>
#include <thread>

int main() {
  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  cex::common::log_json("INFO", "Venues starting", {{"brokers", brokers}});

  cex::venues::app::VenuesLoop loop(brokers);
  loop.start();

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
  }
  return 0;
}
