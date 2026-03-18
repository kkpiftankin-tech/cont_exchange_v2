#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include <chrono>
#include <thread>

#include "app/matching_loop.hpp"

int main() {
  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  const int interval_ms =
      cex::common::Env::get_int("BATCH_INTERVAL_MS", 1000);

  cex::common::log_json("INFO", "Matching starting",
                        {{"brokers", brokers}, {"interval_ms", std::to_string(interval_ms)}});

  cex::matching::app::MatchingLoop loop(brokers, interval_ms);
  loop.start();

  // run forever
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
  }
  return 0;
}
