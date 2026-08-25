#include <utility>
#include <boost/asio.hpp>

#include "app/service.hpp"
#include "cex/common/env.hpp"
#include "infra/kafka/kafka_consumer.hpp"
#include "infra/kafka/kafka_producer.hpp"

using cex::common::Env;
using namespace cex::venue_health;

int main() {
  boost::asio::io_context io;
  boost::asio::signal_set signals(io, SIGINT, SIGTERM);

  std::string brokers = Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  auto circuit_breaker_window = std::chrono::seconds(Env::get_int("CIRCUIT_BREAKER_WINDOW_S", 60));
  auto circuit_breaker_cooldown =
      std::chrono::seconds(Env::get_int("CIRCUIT_BREAKER_COOLDOWN_S", 300));
  int circuit_breaker_error_threshold = Env::get_int("CIRCUIT_BREAKER_ERRORS", 3);
  auto stale_threshold = std::chrono::milliseconds(Env::get_int("STALE_THRESHOLD_MS", 15000));
  domain::Config config{
      circuit_breaker_window,
      circuit_breaker_cooldown,
      static_cast<uint32_t>(circuit_breaker_error_threshold),
      stale_threshold,
  };

  infra::KafkaProducer publisher(brokers);
  app::Service service(publisher, config);
  infra::KafkaConsumer handler(brokers, service);

  signals.async_wait([&](const boost::system::error_code&, int) {
    handler.stop();
    io.stop();
  });

  handler.start();
  io.run();
}
