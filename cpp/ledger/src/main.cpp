#include <grpcpp/grpcpp.h>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/ledger_uc.hpp"
#include "infra/kafka_consumers.hpp"
#include "infra/postgres_repositories.hpp"
#include "transport/grpc_ledger_service.hpp"

int main() {
  const std::string listen_addr =
      cex::common::Env::get_string("LEDGER_GRPC_LISTEN", "0.0.0.0:50053");

  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  const std::string pg_dsn =
      cex::common::Env::get_string("LEDGER_POSTGRES_DSN", "");

  std::shared_ptr<cex::ledger::app::PositionsRepositoryPort> positions_repo;
  std::shared_ptr<cex::ledger::app::LedgerEntriesRepositoryPort> entries_repo;
  std::shared_ptr<cex::ledger::app::HedgeLedgerEntriesRepositoryPort> hedge_entries_repo;
  std::shared_ptr<cex::ledger::infra::PostgresIdempotencyRepository> idempotency_repo;
  // F-12 / IN-009 DoD-6 (PR-F12-3c) — sink for hedge_pnl/fee deltas into
  // PG hedgeflows. nullptr is valid (HedgePnL stays in memory only).
  std::shared_ptr<cex::ledger::app::HedgeflowPnlSinkPort> hedgeflow_pnl_sink;

  if (!pg_dsn.empty()) {
    positions_repo = std::make_shared<cex::ledger::infra::PostgresPositionsRepository>(pg_dsn);
    entries_repo = std::make_shared<cex::ledger::infra::PostgresLedgerEntriesRepository>(pg_dsn);
    hedge_entries_repo = std::make_shared<cex::ledger::infra::PostgresHedgeLedgerEntriesRepository>(pg_dsn);
    idempotency_repo = std::make_shared<cex::ledger::infra::PostgresIdempotencyRepository>(pg_dsn);
    hedgeflow_pnl_sink = std::make_shared<cex::ledger::infra::PostgresHedgeflowPnlSink>(pg_dsn);

    cex::common::log_json("INFO", "PostgreSQL repositories initialized",
                          {{"dsn", pg_dsn},
                           {"hedgeflow_pnl_sink", "ready"}});
  } else {
    cex::common::log_json("WARN", "PostgreSQL DSN not set, persistence disabled");
  }

  cex::ledger::app::LedgerUseCases uc(cex::ledger::app::LedgerUseCases::InitOptions{},
                                       positions_repo, entries_repo, hedge_entries_repo);
  uc.SetIdempotencyRepo(idempotency_repo);
  uc.SetHedgeflowPnlSink(hedgeflow_pnl_sink);

  // Start Kafka consumers in background (batch.outputs, execution.intents, execution.reports, execution.venue)
  cex::ledger::infra::KafkaConsumers consumers(&uc, brokers);
  consumers.start();

  cex::ledger::transport::GrpcLedgerService svc(&uc);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  auto server = builder.BuildAndStart();
  cex::common::log_json("INFO", "Ledger gRPC listening", {{"addr", listen_addr}});
  server->Wait();

  consumers.stop();
  return 0;
}
