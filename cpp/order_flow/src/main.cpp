#include <grpcpp/grpcpp.h>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/order_flow_uc.hpp"
#include "infra/ledger_client.hpp"
#include "infra/orders_kafka_publisher.hpp"
#include "infra/risk_client.hpp"
#include "transport/grpc_order_flow_service.hpp"

int main() {
  const std::string listen_addr =
      cex::common::Env::get_string("ORDER_FLOW_GRPC_LISTEN", "0.0.0.0:50051");

  const std::string risk_addr =
      cex::common::Env::get_string("RISK_GRPC_ADDR", "risk:50052");

  const std::string ledger_addr =
      cex::common::Env::get_string("LEDGER_GRPC_ADDR", "ledger:50053");

  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  cex::common::KafkaProducer producer({.brokers = brokers, .client_id = "order_flow"});
  cex::order_flow::infra::OrdersKafkaPublisher publisher(std::move(producer));

  cex::order_flow::infra::RiskClient risk(risk_addr);
  cex::order_flow::infra::LedgerClient ledger(ledger_addr);

  cex::order_flow::app::OrderFlowUseCases uc(std::move(risk), std::move(ledger), std::move(publisher));
  cex::order_flow::transport::GrpcOrderFlowService svc(&uc);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  cex::common::log_json("INFO", "OrderFlow gRPC listening", {{"addr", listen_addr}});
  server->Wait();
  return 0;
}
