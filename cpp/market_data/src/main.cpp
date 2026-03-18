#include <grpcpp/grpcpp.h>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/market_data_uc.hpp"
#include "infra/kafka_consumer.hpp"
#include "transport/grpc_market_data_service.hpp"

int main() {
  const std::string listen_addr =
      cex::common::Env::get_string("MARKET_DATA_GRPC_LISTEN", "0.0.0.0:50054");

  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  cex::market_data::app::MarketDataUseCases uc;

  cex::market_data::infra::MarketDataKafkaConsumer consumer(&uc, brokers);
  consumer.start();

  cex::market_data::transport::GrpcMarketDataService svc(&uc);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  auto server = builder.BuildAndStart();
  cex::common::log_json("INFO", "MarketData gRPC listening", {{"addr", listen_addr}});
  server->Wait();

  consumer.stop();
  return 0;
}
