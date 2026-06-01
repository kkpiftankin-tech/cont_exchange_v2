#include <grpcpp/grpcpp.h>

#include <exception>
#include <memory>
#include <string>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/order_flow_uc.hpp"
#include "infra/batch_results_consumer.hpp"
#include "infra/ledger_client.hpp"
#include "infra/orders_kafka_publisher.hpp"
#include "infra/postgres/postgres_flow_order_repository.hpp"
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

  // Optional: when set, FlowOrder is persisted to flow_orders + flow_order_legs
  // so the F-04 batch clearing loop picks it up. When empty (legacy / dev
  // compose without PG), order_flow keeps the in-memory + Kafka-only path.
  const std::string pg_dsn =
      cex::common::Env::get_string("ORDER_FLOW_POSTGRES_DSN", "");

  cex::common::KafkaProducer producer({.brokers = brokers, .client_id = "order_flow"});
  cex::order_flow::infra::OrdersKafkaPublisher publisher(std::move(producer));

  cex::order_flow::infra::RiskClient risk(risk_addr);
  cex::order_flow::infra::LedgerClient ledger(ledger_addr);

  std::shared_ptr<cex::order_flow::infra::IFlowOrderRepository> flow_order_repo;
  if (!pg_dsn.empty()) {
    try {
      flow_order_repo =
          std::make_shared<cex::order_flow::infra::PostgresFlowOrderRepository>(pg_dsn);
      cex::common::log_json("INFO",
                            "OrderFlow PostgreSQL writer enabled",
                            {{"dsn_redacted", "set"}});
    } catch (const std::exception& e) {
      cex::common::log_json("ERROR",
                            "OrderFlow failed to init PostgreSQL writer; "
                            "continuing without PG persistence",
                            {{"error", e.what()}});
    }
  } else {
    cex::common::log_json("INFO",
                          "OrderFlow PostgreSQL writer disabled "
                          "(ORDER_FLOW_POSTGRES_DSN not set)",
                          {});
  }

  cex::order_flow::app::OrderFlowUseCases uc(std::move(risk),
                                             std::move(ledger),
                                             std::move(publisher),
                                             std::move(flow_order_repo));
  cex::order_flow::transport::GrpcOrderFlowService svc(&uc);

  // Subscribe to batch.outputs so fills from matching propagate to the
  // in-memory FlowOrder store. Without this, gateway/UI would forever see
  // status=new even after the order is filled in ClickHouse/ledger.
  // See IN-007 BUG-1.
  cex::order_flow::infra::BatchResultsConsumer batch_consumer(&uc, brokers);
  batch_consumer.Start();

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  cex::common::log_json("INFO", "OrderFlow gRPC listening", {{"addr", listen_addr}});
  server->Wait();

  batch_consumer.Stop();
  return 0;
}
