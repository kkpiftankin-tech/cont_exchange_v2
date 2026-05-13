// =============================================================================
// Entrypoint сервиса risk. Поднимает gRPC-сервер RiskService и Kafka-продюсера
// для канала risk.alerts.
// =============================================================================

#include <grpcpp/grpcpp.h>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/risk_uc.hpp"
#include "infra/risk_alerts_publisher.hpp"
#include "transport/grpc_risk_service.hpp"

int main() {
  // Слушаем 50052 по умолчанию. Дёргают этот сервис: order_flow (CheckNewOrder),
  // gateway/operator UI (SetKillSwitch), потенциально matching (post-trade).
  const std::string listen_addr =
      cex::common::Env::get_string("RISK_GRPC_LISTEN", "0.0.0.0:50052");

  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  // Алерты публикуем в risk.alerts (consumer'ом будет observability).
  cex::common::KafkaProducer producer({.brokers = brokers, .client_id = "risk"});
  cex::risk::infra::RiskAlertsPublisher pub(std::move(producer));

  cex::risk::app::RiskUseCases uc(std::move(pub));
  cex::risk::transport::GrpcRiskService svc(&uc);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  auto server = builder.BuildAndStart();
  cex::common::log_json("INFO", "Risk gRPC listening", {{"addr", listen_addr}});
  server->Wait();
  return 0;
}
