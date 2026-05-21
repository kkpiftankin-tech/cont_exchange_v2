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
  std::string brokers = Env::get_string("KAFKA_BROKERS", "redpanda:9092");
  std::string clickhouse_url = Env::get_string("CLICKHOUSE_URL", "clickhouse:8123");
  std::string grpc_url = Env::get_string("OBSERVABILITY_GRPC_URL", "observability:50051");

  log_json("INFO", "Observability starting",
           {{"brokers", brokers}, {"clickhouse", clickhouse_url}, {"grpc", grpc_url}});

  infra::AlertChannel channel;
  app::Service service{channel};
  infra::KafkaConsumer consumer{brokers, service};

  transport::ObservabilityService grpc_service{channel, clickhouse_url};

  grpc::ServerBuilder builder;
  builder.AddListeningPort(grpc_url, grpc::InsecureServerCredentials());
  builder.RegisterService(&grpc_service);

  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  server->Wait();
}
