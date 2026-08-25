// ============================================================================
// backtest/main.cpp — entry point сервиса backtest (F-15 backtest/replay).
//
// Что делает:
//   - PG repositories: replay sessions, RBAC, replay configs.
//   - ClickHouse читает historical batchresults/marketdata для replay input.
//   - Запускает Kafka consumers для replay.commands (operator-driven) и
//     venue events (для parity check'ов).
//   - Publisher для replay.results (progress/completed/failed/cancelled).
//   - In-memory shadow ledger (in_memory_shadow_ledger.cpp) — изолирует
//     replay PnL от live ledger state (ADR-015 sim/live isolation pattern).
//   - gRPC ReplayBatchExecutor (для isolation matching service).
//   - HTTP routes (Crow):
//       * /healthz                 — liveness probe.
//       * /metrics                  — Prometheus.
//       * Replay retry session API.
//
// CLAUDE.md §15 §17: replay должен deterministic — тот же input даёт
// тот же output. Replay использует isolation matching solver (cpp/matching
// GrpcIsolationSolverService) чтобы не trogать live state.
// ============================================================================

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/backtest_uc.hpp"
#include "app/cancel_replay_session_uc.hpp"
#include "app/create_replay_session_uc.hpp"
#include "app/historical_batch_loader_uc.hpp"
#include "app/lob_fob_replay_uc.hpp"
#include "app/quality_metrics_uc.hpp"
#include "app/replay_config_snapshot.hpp"
#include "app/replay_runtime_metrics.hpp"
#include "app/retry_replay_session_uc.hpp"
#include "app/run_replay_session_uc.hpp"
#include "app/shadow_namespace_uc.hpp"
#include "app/venue_replay_uc.hpp"
#include "crow.h"
#include "infra/clickhouse_storage.hpp"
#include "infra/grpc_replay_batch_executor.hpp"
#include "infra/http/retry_session_handler.hpp"
#include "infra/in_memory_shadow_ledger.hpp"
#include "infra/kafka_consumer.hpp"
#include "infra/kafka_replay_event_publisher.hpp"
#include "infra/postgres/postgres_rbac_repository.hpp"
#include "infra/postgres/postgres_replay_config_repository.hpp"
#include "infra/postgres/postgres_replay_session_repository.hpp"
#include "infra/replay_commands_kafka_consumer.hpp"
#include "infra/venue_kafka_consumer.hpp"
// F15-PERSIST: routing wrappers + ephemeral registry
#include "infra/ephemeral_session_registry.hpp"
#include "infra/routing_replay_session_repository.hpp"
#include "infra/routing_summary_store.hpp"
#include "infra/routing_agent_log_writer.hpp"

int main() {
  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");
  const auto metrics_port = static_cast<uint16_t>(
      cex::common::Env::get_int("BACKTEST_METRICS_PORT", 8087));
  cex::backtest::app::ReplayRuntimeMetrics replay_metrics;

  cex::backtest::infra::ClickHouseConfig ch_cfg;
  ch_cfg.url = cex::common::Env::get_string("BACKTEST_CLICKHOUSE_URL", "http://clickhouse:8123");
  ch_cfg.database = cex::common::Env::get_string("BACKTEST_CLICKHOUSE_DB", "default");
  // IN-007 BUG-2 part 2: backtest must NOT write to live `batchresults` /
  // `fills` tables. Previously default = "batchresults"/"fills" caused
  // market_data + backtest to double-insert every live batch (2 rows in
  // `fills` per batch). Backtest now writes to its own namespace; replay
  // / quality-check consumers should read from these tables explicitly.
  ch_cfg.batchresults_table =
      cex::common::Env::get_string("BACKTEST_CLICKHOUSE_BATCHRESULTS_TABLE", "backtest_batchresults");
  ch_cfg.fills_table =
      cex::common::Env::get_string("BACKTEST_CLICKHOUSE_FILLS_TABLE", "backtest_fills");
  ch_cfg.replay_agentlogs_table = cex::common::Env::get_string(
      "BACKTEST_CLICKHOUSE_REPLAY_AGENTLOGS_TABLE", "replay_agentlogs");
  ch_cfg.replay_agentlogs_retention_days = std::max(
      1, cex::common::Env::get_int("BACKTEST_CLICKHOUSE_REPLAY_AGENTLOGS_RETENTION_DAYS", 90));
  ch_cfg.user = cex::common::Env::get_string("BACKTEST_CLICKHOUSE_USER", "default");
  ch_cfg.password = cex::common::Env::get_string("BACKTEST_CLICKHOUSE_PASSWORD", "");
  ch_cfg.timeout_ms = cex::common::Env::get_int("BACKTEST_CLICKHOUSE_TIMEOUT_MS", 3000);

  cex::backtest::infra::ClickHouseReplayStorage storage(ch_cfg, &replay_metrics);
  if (!storage.EnsureSchema()) {
    cex::common::log_json("WARN", "Backtest ClickHouse schema initialization failed");
  }

  // F04-BACKTEST: batch.outputs -> ClickHouse
  cex::backtest::app::BacktestUseCases batch_uc(&storage, &storage);
  cex::backtest::infra::BacktestKafkaConsumer batch_consumer(&batch_uc, brokers);
  batch_consumer.start();

  // F11-BACKTEST-1: venue.snapshots + venue.liquidity.fob -> ClickHouse
  cex::backtest::app::VenueReplayUseCases venue_uc(&storage);
  cex::backtest::infra::VenueKafkaConsumer venue_consumer(&venue_uc, brokers);
  venue_consumer.start();

  // F11-BACKTEST-2: LOB->FOB replay pipeline (available for on-demand replay runs).
  cex::backtest::app::LobFobReplayUseCases replay_uc(
      static_cast<cex::backtest::app::IReplayReader*>(&storage));
  (void)replay_uc;  // Ready for API/CLI trigger; not started automatically.

  // F11-BACKTEST-3: LOB->FOB quality metrics on historical data.
  cex::backtest::app::QualityMetricsUseCases quality_uc(
      static_cast<cex::backtest::app::IReplayReader*>(&storage),
      static_cast<cex::backtest::app::IQualityMetricsStorage*>(&storage));
  (void)quality_uc;  // Ready for API/CLI trigger; not started automatically.

  // F15-BACKTEST-1: chunked, deterministic loader of historical batches,
  // fills and marketdata snapshots from ClickHouse for replay.
  cex::backtest::app::HistoricalBatchLoaderUseCases hist_loader_uc(
      static_cast<cex::backtest::app::IHistoricalBatchLoader*>(&storage));
  (void)hist_loader_uc;  // Ready for API/CLI trigger; not started automatically.

  // F15-BACKTEST-2: config + materialization snapshot loader. Reads
  // solver/risk/fee/reward configuration documents from PostgreSQL (or
  // accepts inline overrides) and produces a deterministic JSON snapshot
  // that gets persisted on the ReplaySession for reproducible re-runs.
  const std::string pg_conn = cex::common::Env::get_string(
      "BACKTEST_POSTGRES_DSN", "");
  std::unique_ptr<cex::backtest::infra::PostgresReplayConfigRepository> pg_config_repo;
  if (!pg_conn.empty()) {
    pg_config_repo =
        std::make_unique<cex::backtest::infra::PostgresReplayConfigRepository>(pg_conn);
    if (!pg_config_repo->EnsureSchema()) {
      cex::common::log_json("WARN", "Backtest replay config schema initialization failed");
    }
  }
  cex::backtest::app::ReplayConfigSnapshotBuilder config_snapshot_builder(
      pg_config_repo.get());
  (void)config_snapshot_builder;  // Ready for API/CLI trigger; not started automatically.

  // F15-API-1 + F15-API-2: replay session repository and create-session use
  // case wiring. CreateReplaySession validates launch inputs, checks that at
  // least one historical batch exists in range, materializes a deterministic
  // config snapshot and inserts replay_sessions row with status=pending.
  std::unique_ptr<cex::backtest::infra::PostgresReplaySessionRepository> pg_session_repo;
  if (!pg_conn.empty()) {
    pg_session_repo =
        std::make_unique<cex::backtest::infra::PostgresReplaySessionRepository>(pg_conn);
    if (!pg_session_repo->EnsureSchema()) {
      cex::common::log_json("WARN", "Backtest replay session schema initialization failed");
    }
  }
  std::shared_ptr<cex::backtest::app::RbacEngine> rbac_engine;
  if (!pg_conn.empty()) {
    try {
      auto rbac_conn = std::make_shared<pqxx::connection>(pg_conn);
      auto rbac_repo =
          std::make_shared<cex::backtest::infra::postgres::PostgresRbacRepository>(
              rbac_conn);
      rbac_engine = std::make_shared<cex::backtest::app::RbacEngine>(rbac_repo);
    } catch (const std::exception& ex) {
      cex::common::log_json("WARN", "Backtest RBAC initialization failed",
                            {{"error", ex.what()}});
    }
  }

  // F15-PERSIST: in-memory registry for ephemeral (persist=false) sessions +
  // transparent routing wrappers that intercept all port calls and route
  // based on the session's persist flag.  All use-case deps below use the
  // routing wrappers so the use cases themselves stay unchanged.
  auto ephemeral_registry =
      std::make_unique<cex::backtest::infra::EphemeralSessionRegistry>();
  auto routing_session_repo =
      std::make_unique<cex::backtest::infra::RoutingReplaySessionRepository>(
          pg_session_repo.get(), ephemeral_registry.get());
  auto routing_summary_store =
      std::make_unique<cex::backtest::infra::RoutingReplaySummaryStore>(
          static_cast<cex::backtest::app::IReplaySummaryStore*>(pg_session_repo.get()),
          ephemeral_registry.get());
  auto routing_agent_log_writer =
      std::make_unique<cex::backtest::infra::RoutingAgentLogWriter>(
          static_cast<cex::backtest::app::IAgentLogWriter*>(&storage),
          ephemeral_registry.get());

  cex::backtest::app::CreateReplaySession::Dependencies create_replay_deps;
  create_replay_deps.session_repo = routing_session_repo.get();
  create_replay_deps.config_builder = &config_snapshot_builder;
  create_replay_deps.historical_loader =
      static_cast<cex::backtest::app::IHistoricalBatchLoader*>(&storage);
  create_replay_deps.rbac_engine = rbac_engine.get();
  cex::backtest::app::CreateReplaySession create_replay_session_uc(create_replay_deps);

  // F15-BACKTEST-3: in-process shadow ledger + per-session namespace
  // initializer. The in-memory ledger lives entirely inside this process,
  // guaranteeing isolation from the production Collateral Ledger.
  cex::backtest::infra::InMemoryShadowLedger shadow_ledger(&replay_metrics);
  cex::backtest::app::ShadowNamespaceInitializer shadow_namespace_uc(&shadow_ledger);
  (void)shadow_namespace_uc;  // Ready for API/CLI trigger; not started automatically.

  // F15-DATA-4: Kafka producer for replay progress/lifecycle on replay.results.
  cex::common::KafkaProducer replay_results_producer(
      {.brokers = brokers, .client_id = "backtest-replay-results"});
  cex::backtest::infra::KafkaReplayEventPublisher replay_event_publisher(
      std::move(replay_results_producer),
      cex::common::Env::get_string("REPLAY_RESULTS_TOPIC", "replay.results"),
      &replay_metrics);

  // F15-API-5: CancelReplaySession + Kafka consumer for replay.commands.
  // Pending cancels complete immediately and publish a cancelled lifecycle
  // event; running cancels set a session-scoped token that RunReplaySession
  // observes at the next safe point.
  cex::backtest::app::CancelReplaySession::Dependencies cancel_deps;
  cancel_deps.session_repo = routing_session_repo.get();
  cancel_deps.summary_store = routing_summary_store.get();
  cancel_deps.event_publisher = &replay_event_publisher;
  cex::backtest::app::CancelReplaySession cancel_replay_session_uc(cancel_deps);

  cex::backtest::app::RetryReplaySessionUseCase::Dependencies retry_deps;
  retry_deps.session_repo = routing_session_repo.get();
  cex::backtest::app::RetryReplaySessionUseCase retry_replay_session_uc(retry_deps);

  cex::backtest::app::RunReplaySession::Dependencies run_replay_deps;
  run_replay_deps.session_repo = routing_session_repo.get();
  run_replay_deps.config_builder = &config_snapshot_builder;
  run_replay_deps.shadow_init = &shadow_namespace_uc;
  run_replay_deps.shadow_ledger = &shadow_ledger;
  run_replay_deps.history_loader = &hist_loader_uc;
  run_replay_deps.agent_log_writer = routing_agent_log_writer.get();
  run_replay_deps.summary_store = routing_summary_store.get();
  run_replay_deps.event_publisher = &replay_event_publisher;
  run_replay_deps.runtime_metrics = &replay_metrics;
  run_replay_deps.cancellation_token = &cancel_replay_session_uc;
  run_replay_deps.agent_log_reader =
      static_cast<cex::backtest::app::IAgentLogReader*>(&storage);
  run_replay_deps.rbac_engine = rbac_engine.get();
  const std::string matching_target = cex::common::Env::get_string(
      "MATCHING_ISOLATION_GRPC_TARGET", "matching:50053");
  const std::string risk_target = cex::common::Env::get_string(
      "RISK_GRPC_ADDR", "risk:50052");
  cex::backtest::infra::GrpcReplayBatchExecutor replay_batch_executor(
      fob::matching::v1::Solver::NewStub(grpc::CreateChannel(
          matching_target, grpc::InsecureChannelCredentials())),
      fob::risk::v1::RiskService::NewStub(grpc::CreateChannel(
          risk_target, grpc::InsecureChannelCredentials())),
      &shadow_ledger,
      {.rpc_timeout_ms = cex::common::Env::get_int("BACKTEST_REPLAY_RPC_TIMEOUT_MS", 3000),
       .historical_curve_lookback_ms = cex::common::Env::get_int(
           "BACKTEST_REPLAY_CURVE_LOOKBACK_MS", 60000)},
      static_cast<cex::backtest::app::IReplayReader*>(&storage));
  run_replay_deps.batch_executor = &replay_batch_executor;
  cex::common::log_json(
      "INFO",
      "F15 replay batch executor configured",
      {{"matching_target", matching_target}, {"risk_target", risk_target}});
  cex::backtest::app::RunReplaySession run_replay_session_uc(run_replay_deps);

  cex::backtest::infra::ReplayCommandsKafkaConsumer::UseCases replay_cmd_ucs;
  replay_cmd_ucs.cancel = &cancel_replay_session_uc;
  replay_cmd_ucs.create = &create_replay_session_uc;
  replay_cmd_ucs.run = &run_replay_session_uc;
  replay_cmd_ucs.retry = &retry_replay_session_uc;
  replay_cmd_ucs.events = &replay_event_publisher;
  replay_cmd_ucs.ephemeral_registry = ephemeral_registry.get();
  cex::backtest::infra::ReplayCommandsKafkaConsumer replay_commands_consumer(
      replay_cmd_ucs, brokers,
      cex::common::Env::get_string("REPLAY_COMMANDS_TOPIC", "replay.commands"));
  replay_commands_consumer.start();

  // F15-API-3: HTTP API for RetryReplaySession use case
  cex::backtest::infra::RetrySessionHandler retry_session_handler(&retry_replay_session_uc);

  crow::SimpleApp api_app;

  CROW_ROUTE(api_app, "/healthz")([] { return crow::response(200, "ok"); });

  CROW_ROUTE(api_app, "/v1/retry-session")
      .methods("POST"_method)
      ([&retry_session_handler](const crow::request& req) {
        return retry_session_handler.HandleRetryRequest(req);
      });

  const auto api_port = static_cast<uint16_t>(
      cex::common::Env::get_int("BACKTEST_API_PORT", 8080));
  [[maybe_unused]] auto api_server = api_app.port(api_port).run_async();

  crow::SimpleApp metrics_app;
  CROW_ROUTE(metrics_app, "/healthz")([] { return crow::response(200, "ok"); });
  CROW_ROUTE(metrics_app, "/metrics")([&replay_metrics] {
    crow::response response;
    response.code = 200;
    response.set_header("Content-Type",
                        "text/plain; version=0.0.4; charset=utf-8");
    response.write(replay_metrics.RenderPrometheus());
    return response;
  });
  [[maybe_unused]] auto metrics_server = metrics_app.port(metrics_port).run_async();

  cex::common::log_json("INFO", "Backtest service started",
                        {{"clickhouse_db", ch_cfg.database},
                         {"brokers", brokers},
                         {"metrics_port", std::to_string(metrics_port)}});

  // Block until signal (consumer loops run in background threads).
  std::this_thread::sleep_until(std::chrono::time_point<std::chrono::steady_clock>::max());

  replay_commands_consumer.stop();
  venue_consumer.stop();
  batch_consumer.stop();
  return 0;
}
