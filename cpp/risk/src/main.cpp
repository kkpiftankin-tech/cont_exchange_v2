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
#include "infra/risk_snapshot_repository.hpp"
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

  // F-06 (T-F06-031/032): PostgreSQL DAO для post-trade margin snapshots.
  // RISK_POSTGRES_DSN пуст → snapshot-pipeline отключён (degraded mode):
  // OnBatchResult логирует diagnostics, GetRiskSnapshot отдаёт empty.
  const std::string pg_dsn =
      cex::common::Env::get_string("RISK_POSTGRES_DSN", "");
  cex::risk::infra::RiskSnapshotRepository snapshot_repo(pg_dsn);
  if (snapshot_repo.enabled()) {
    uc.SetSnapshotRepository(&snapshot_repo);
    cex::common::log_json("INFO", "Risk margin snapshot persistence enabled");
  } else {
    cex::common::log_json(
        "WARN", "RISK_POSTGRES_DSN not set or libpqxx unavailable; "
                "margin snapshots disabled");
  }

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
