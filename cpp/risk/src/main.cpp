// ============================================================================
// risk/main.cpp — entry point сервиса risk (F-07 + F-08).
//
// Что делает:
//   - Запускает gRPC RiskService (PreTradeCheck, PostTradeCheck, PreHedgeCheck,
//     KillSwitch, и др. — см. cpp/risk/src/app/risk_uc.cpp).
//   - Запускает Kafka consumer для venue.health, batch.outputs, execution.reports
//     (см. cpp/risk/src/infra/kafka_consumer.cpp).
//   - Publishes risk.alerts через RiskAlertsPublisher.
// ============================================================================
#include <grpcpp/grpcpp.h>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/risk_uc.hpp"
#include "infra/risk_alerts_publisher.hpp"
#include "infra/kafka_consumer.hpp"
#include "transport/grpc_risk_service.hpp"

int main() {
  const std::string listen_addr =
      cex::common::Env::get_string("RISK_GRPC_LISTEN", "0.0.0.0:50052");

  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  // Producer для risk.alerts (kill-switch, breach notifications).
  cex::common::KafkaProducer producer({.brokers = brokers, .client_id = "risk"});
  cex::risk::infra::RiskAlertsPublisher pub(std::move(producer));

  // Use cases + gRPC transport wiring.
  cex::risk::app::RiskUseCases uc(std::move(pub));
  cex::risk::transport::GrpcRiskService svc(&uc);

  // Consumer — background threads для venue.health / batch.outputs / execution.reports.
  cex::risk::infra::KafkaConsumer consumer(brokers, uc);
  consumer.start();

  // gRPC server для PreTradeCheck etc. InsecureCredentials — dev only.
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  auto server = builder.BuildAndStart();
  cex::common::log_json("INFO", "Risk gRPC listening", {{"addr", listen_addr}});
  // Wait() blocks main thread; consumer threads продолжают параллельно.
  server->Wait();
  return 0;
}
