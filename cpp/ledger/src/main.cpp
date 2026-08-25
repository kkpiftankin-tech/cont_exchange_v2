// ============================================================================
// ledger/main.cpp — entry point сервиса ledger (F-02 / F-04 / F-06 / F-12).
//
// Что делает:
//   - Инициализирует PG repositories для positions, ledger_entries,
//     hedge_entries, idempotency, hedgeflow PnL sink (F-12 DoD-6).
//   - Запускает gRPC LedgerService (Reserve/Release/Apply/GetBalances/etc).
//   - Запускает Kafka consumers (batch.outputs / execution.intents /
//     execution.venue / execution.reports).
//
// CLAUDE.md §17: ledger — источник истины по balances/positions.
// ============================================================================
#include <grpcpp/grpcpp.h>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "cex/common/kafka.hpp"

#include "app/ledger_uc.hpp"
#include "infra/kafka_consumers.hpp"
#include "infra/positions_update_publisher.hpp"
#include "infra/postgres_repositories.hpp"
#include "transport/grpc_ledger_service.hpp"

int main() {
  const std::string listen_addr =
      cex::common::Env::get_string("LEDGER_GRPC_LISTEN", "0.0.0.0:50053");

  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  // Empty DSN → in-memory mode (CLAUDE.md §14: targeted state in PG для prod).
  const std::string pg_dsn =
      cex::common::Env::get_string("LEDGER_POSTGRES_DSN", "");

  // Optional repositories — все опциональны (nullptr = feature disabled).
  std::shared_ptr<cex::ledger::app::PositionsRepositoryPort> positions_repo;
  std::shared_ptr<cex::ledger::app::LedgerEntriesRepositoryPort> entries_repo;
  std::shared_ptr<cex::ledger::app::HedgeLedgerEntriesRepositoryPort> hedge_entries_repo;
  std::shared_ptr<cex::ledger::infra::PostgresIdempotencyRepository> idempotency_repo;
  // F-12 / IN-009 DoD-6 (PR-F12-3c) — sink for hedge_pnl/fee deltas into
  // PG hedgeflows. nullptr is valid (HedgePnL stays in memory only).
  std::shared_ptr<cex::ledger::app::HedgeflowPnlSinkPort> hedgeflow_pnl_sink;

  // Если DSN задан — инициализируем PG-репозитории. Иначе остаёмся в
  // in-memory режиме (теряем state при restart, OK для dev).
  // F-06 (T-F06-020/021) — F-06 positions/accounts repositories.
  std::shared_ptr<cex::ledger::app::PositionRepositoryPort> position_repo;
  std::shared_ptr<cex::ledger::app::AccountRepositoryPort> account_repo;
  std::shared_ptr<cex::ledger::app::PositionAccountTxPort> position_account_tx;
  // F-06 P0-A (T-F06-070, ADR-044) — mirror reserve/release into PG `accounts`.
  std::shared_ptr<cex::ledger::app::AccountReserveTxPort> account_reserve_tx;

  if (!pg_dsn.empty()) {
    // ADR-045: единый пул PG-соединений на сервис (вместо нового
    // pqxx::connection на каждый вызов). Размер — через env.
    const std::size_t pool_size = static_cast<std::size_t>(
        cex::common::Env::get_int("LEDGER_PG_POOL_SIZE", 16));
    auto pg_pool = cex::ledger::infra::MakeLedgerPgPool(pg_dsn, pool_size);

    positions_repo = std::make_shared<cex::ledger::infra::PostgresPositionsRepository>(pg_pool);
    entries_repo = std::make_shared<cex::ledger::infra::PostgresLedgerEntriesRepository>(pg_pool);
    hedge_entries_repo = std::make_shared<cex::ledger::infra::PostgresHedgeLedgerEntriesRepository>(pg_pool);
    idempotency_repo = std::make_shared<cex::ledger::infra::PostgresIdempotencyRepository>(pg_pool);
    hedgeflow_pnl_sink = std::make_shared<cex::ledger::infra::PostgresHedgeflowPnlSink>(pg_pool);
    position_repo = std::make_shared<cex::ledger::infra::PostgresPositionRepository>(pg_pool);
    account_repo = std::make_shared<cex::ledger::infra::PostgresAccountRepository>(pg_pool);
    position_account_tx = std::make_shared<cex::ledger::infra::PostgresPositionAccountTx>(pg_pool);
    account_reserve_tx = std::make_shared<cex::ledger::infra::PostgresAccountReserveTx>(pg_pool);

    cex::common::log_json("INFO", "PostgreSQL repositories initialized",
                          {{"dsn", pg_dsn},
                           {"pool_size", std::to_string(pool_size)},
                           {"hedgeflow_pnl_sink", "ready"},
                           {"f06_positions_accounts", "ready"}});
  } else {
    cex::common::log_json("WARN", "PostgreSQL DSN not set, persistence disabled");
  }

  // LedgerUseCases с seed_demo_state=false (default) — пустой ledger для prod.
  cex::ledger::app::LedgerUseCases uc(cex::ledger::app::LedgerUseCases::InitOptions{},
                                       positions_repo, entries_repo, hedge_entries_repo);
  uc.SetIdempotencyRepo(idempotency_repo);
  uc.SetHedgeflowPnlSink(hedgeflow_pnl_sink);
  uc.SetPositionRepo(position_repo);
  uc.SetAccountRepo(account_repo);
  uc.SetPositionAccountTx(position_account_tx);
  uc.SetAccountReserveTx(account_reserve_tx);

  // F-06 / F6-5 (T-F06-072, ADR-046): producer топика positions.update.
  // После успешного ApplyBatchResult ledger эмиттит лёгкий invalidation-сигнал
  // (key=user_id) → ws-gateway реагрегирует снимок и пушит клиенту по WS.
  auto positions_update_publisher =
      std::make_shared<cex::ledger::infra::PositionsUpdatePublisher>(
          cex::common::KafkaProducer(
              cex::common::KafkaConfig{.brokers = brokers, .client_id = "ledger"}));
  uc.SetPositionsUpdatePublisher(positions_update_publisher);

  // Start Kafka consumers in background (batch.outputs, execution.intents, execution.reports, execution.venue)
  // ВАЖНО: ApplyBatchResult идempotent через idempotency_repo → safe при rebalance.
  cex::ledger::infra::KafkaConsumers consumers(&uc, brokers);
  consumers.start();

  cex::ledger::transport::GrpcLedgerService svc(&uc);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  auto server = builder.BuildAndStart();
  cex::common::log_json("INFO", "Ledger gRPC listening", {{"addr", listen_addr}});
  server->Wait();

  // Graceful shutdown: stop consumer threads до завершения процесса.
  consumers.stop();
  return 0;
}
