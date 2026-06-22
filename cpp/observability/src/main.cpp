// ============================================================================
// observability/main.cpp — entry point сервиса observability (F-17).
//
// Что делает:
//   - Подписывается на ключевые Kafka топики через KafkaConsumer:
//     risk.alerts, batch.outputs, execution.venue, venue.health и др.
//   - Эмиттит structured summaries в JSON (см. infra/channel/AlertChannel).
//   - Поднимает gRPC ObservabilityService для query'ов (метрики, история).
//   - Sink аналитики в ClickHouse через clickhouse_url.
// ============================================================================
#include <grpcpp/server_builder.h>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "infra/channel/alert_channel.hpp"
#include "infra/kafka/kafka_consumer.hpp"
#include "transport/grpc/observability_service.hpp"

using cex::common::Env;
using cex::common::log_json;
using namespace cex::observability;

int main() {
  // Конфиг через env — никакого хардкода (CLAUDE.md §12.3).
  std::string brokers = Env::get_string("KAFKA_BROKERS", "redpanda:9092");
  std::string clickhouse_url = Env::get_string("CLICKHOUSE_URL", "clickhouse:8123");
  std::string grpc_url = Env::get_string("OBSERVABILITY_GRPC_URL", "observability:50051");

  log_json("INFO", "Observability starting",
           {{"brokers", brokers}, {"clickhouse", clickhouse_url}, {"grpc", grpc_url}});

  // Channel — общий sink alert'ов между Service и gRPC handler.
  infra::AlertChannel channel;
  // Service — application layer, обрабатывает входящие Kafka events.
  app::Service service{channel};
  // Consumer запускает background thread'ы с subscribe + poll loop.
  infra::KafkaConsumer consumer{brokers, service};

  // gRPC service для query API (operator UI, metrics dashboard).
  transport::ObservabilityService grpc_service{channel, clickhouse_url};

  // Стандартный grpc::ServerBuilder pattern. InsecureServerCredentials —
  // dev-only (CLAUDE.md §22 — production требует mTLS).
  grpc::ServerBuilder builder;
  builder.AddListeningPort(grpc_url, grpc::InsecureServerCredentials());
  builder.RegisterService(&grpc_service);

  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  // Wait() blocks main thread до shutdown signal. Kafka consumer + service
  // background threads продолжают работать параллельно.
  server->Wait();
}
