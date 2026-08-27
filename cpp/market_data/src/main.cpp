// ============================================================================
// market_data/main.cpp — entry point сервиса market_data (F-05).
//
// Что делает:
//   - Инициализирует ClickHouse-схему для batchresults, fills, execution_venue,
//     execution_reports, marketdata, liquidity_curves.
//   - Запускает Kafka consumer для marketdata.raw, batch.outputs, и др.
//     Persists records в ClickHouse для analytics / replay (F-15).
//   - Запускает gRPC MarketDataService (GetLastTicker, GetLiquidityCurve, ...).
//   - In-memory кэш текущих liquidity curves для fast access из solver/matching.
// ============================================================================
#include <grpcpp/grpcpp.h>
#include <algorithm>

#include "cex/common/defer.hpp"
#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/market_data_uc.hpp"
#include "infra/clickhouse/clickhouse_liquidity_curve_storage.hpp"
#include "infra/clickhouse/clickhouse_vector_segment_storage.hpp"  // F-05A (T-F05A-206)
#include "infra/clickhouse/clickhouse_vector_clearing_storage.hpp"  // F-05A (T-F05A-305)
#include "infra/clickhouse/order_book_storage.hpp"
#include "infra/clickhouse/ch_snapshot_storage.hpp"
#include "infra/clickhouse_storage.hpp"
#include "infra/kafka_consumer.hpp"
#include "infra/kafka_snapshots_producer.hpp"
#include "infra/kafka_vectorized_producer.hpp"  // F-05A (T-F05A-205)
#include "infra/liquidity_curve_memory_storage.hpp"
#include "infra/market_data_stream_hub.hpp"
#include "infra/order_book_channel.hpp"
#include "infra/postgres/pg_market_data_config.hpp"
#include "transport/grpc_market_data_service.hpp"

int main() {
  const std::string listen_addr =
      cex::common::Env::get_string("MARKET_DATA_GRPC_LISTEN", "0.0.0.0:50054");
  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  // ClickHouse config — все таблицы overridable env-vars для разделения
  // sample/replay namespaces (BACKTEST_CLICKHOUSE_* vs CLICKHOUSE_* в F-15).
  cex::market_data::infra::ClickHouseConfig ch_cfg;
  ch_cfg.host     = cex::common::Env::get_string("CLICKHOUSE_HOST", "clickhouse");
  ch_cfg.port     = cex::common::Env::get_int("CLICKHOUSE_PORT", 8123);
  ch_cfg.tcp_port = cex::common::Env::get_int("CLICKHOUSE_TCP_PORT", 9000);
  ch_cfg.database = cex::common::Env::get_string("CLICKHOUSE_DB", "default");
  ch_cfg.batchresults_table =
      cex::common::Env::get_string("CLICKHOUSE_BATCHRESULTS_TABLE", "batchresults");
  ch_cfg.fills_table =
      cex::common::Env::get_string("CLICKHOUSE_FILLS_TABLE", "fills");
  ch_cfg.execution_venue_table =
      cex::common::Env::get_string("CLICKHOUSE_EXECUTION_VENUE_TABLE", "execution_venue");
  ch_cfg.execution_reports_table =
      cex::common::Env::get_string("CLICKHOUSE_EXECUTION_REPORTS_TABLE", "execution_reports");
  ch_cfg.marketdata_table =
      cex::common::Env::get_string("CLICKHOUSE_MARKETDATA_TABLE", "marketdata");
  ch_cfg.liquidity_curves_table =
      cex::common::Env::get_string("CLICKHOUSE_LIQUIDITY_CURVES_TABLE", "liquidity_curves");
  ch_cfg.liquidity_curves_retention_days = std::max(
      1, cex::common::Env::get_int("CLICKHOUSE_LIQUIDITY_CURVES_RETENTION_DAYS", 90));
  ch_cfg.user       = cex::common::Env::get_string("CLICKHOUSE_USER", "default");
  ch_cfg.password   = cex::common::Env::get_string("CLICKHOUSE_PASSWORD", "");
  ch_cfg.timeout_ms = cex::common::Env::get_int("CLICKHOUSE_TIMEOUT_MS", 3000);

  // ── Storage backends ─────────────────────────────────────────────────────
  cex::market_data::infra::ClickHouseBatchStorage batch_storage(ch_cfg);
  if (!batch_storage.EnsureSchema()) {
    cex::common::log_json("WARN", "ClickHouse batch schema init failed");
  }

  cex::market_data::infra::clickhouse::OrderBookStorage ob_storage(ch_cfg);
  cex::market_data::infra::clickhouse::ClickHouseLiquidityCurveStorage curve_storage(ch_cfg);
  curve_storage.EnsureSchema();

  // F-05: snapshot + effective spread storage
  cex::market_data::infra::clickhouse::ChSnapshotStorage snapshot_storage(ch_cfg);
  if (!snapshot_storage.EnsureSchema()) {
    cex::common::log_json("WARN", "ClickHouse snapshot schema init failed");
  }

  // ── Kafka producers (F-05) ───────────────────────────────────────────────
  cex::market_data::infra::KafkaSnapshotsProducer snapshots_producer(brokers);
  cex::market_data::infra::KafkaVectorizedProducer vectorized_producer(brokers);  // F-05A
  cex::market_data::infra::KafkaRiskAlertPublisher risk_publisher(brokers);

  // ── In-memory channels ───────────────────────────────────────────────────
  cex::market_data::infra::OrderBookChannel ob_channel;
  auto _ = cex::common::Defer([&ob_channel] { ob_channel.Close(); });

  cex::market_data::infra::LiquidityCurveMemoryStorage memory_curve_storage;

  // F-05: hub для трансляции в gRPC-стримы
  cex::market_data::infra::MarketDataStreamHub stream_hub;

  // F-05: kill-switch через marketdata_config.isActive (PostgreSQL)
  const std::string pg_conn = cex::common::Env::get_string(
      "POSTGRES_CONN",
      "host=postgres port=5432 dbname=exchange user=exchange password=exchange");
  cex::market_data::infra::PgMarketDataConfig pg_config(pg_conn);

  // ── Use Cases ────────────────────────────────────────────────────────────
  cex::market_data::app::MarketDataUseCases uc(
      &batch_storage,
      &batch_storage,       // IExecutionVenueStorage
      &ob_storage,
      &ob_channel,
      &memory_curve_storage,
      &curve_storage,
      &snapshot_storage,    // ISnapshotStorage     (F-05)
      &snapshot_storage,    // IEffectiveSpreadStorage (F-05, same object)
      &snapshots_producer,  // ISnapshotPublisher   (F-05)
      &risk_publisher,      // IRiskAlertPublisher  (F-05)
      &stream_hub,          // MarketDataStreamHub  (F-05)
      &pg_config,           // kill-switch via marketdata_config (F-05)
      &vectorized_producer  // IVectorizedPublisher (F-05A, T-F05A-205)
  );

  // ── F-05A (T-F05A-206): CH persist векторных сегментов ───────────────────
  cex::market_data::infra::clickhouse::ClickHouseVectorSegmentStorage
      vector_segment_storage(ch_cfg);
  vector_segment_storage.EnsureSchema();
  uc.SetVectorSegmentStorage(&vector_segment_storage);

  cex::market_data::infra::clickhouse::ClickHouseVectorClearingStorage
      vector_clearing_storage(ch_cfg);
  vector_clearing_storage.EnsureSchema();
  uc.SetVectorClearingResultStorage(&vector_clearing_storage);

  // ── F-05: стартуем stale sweeper ─────────────────────────────────────────
  uc.StartStaleSweeper();

  // ── Kafka consumer ────────────────────────────────────────────────────────
  cex::market_data::infra::MarketDataKafkaConsumer consumer(&uc, brokers);
  consumer.start();

  // ── gRPC server ───────────────────────────────────────────────────────────
  cex::market_data::transport::GrpcMarketDataService svc(
      &uc, &ob_channel, &stream_hub,
      &snapshot_storage,   // ISnapshotStorage     → GetMarketDataHistory
      &snapshot_storage    // IEffectiveSpreadStorage → GetEffectiveSpread
  );

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  auto server = builder.BuildAndStart();
  cex::common::log_json("INFO", "MarketData gRPC listening", {{"addr", listen_addr}});
  server->Wait();

  consumer.stop();
  uc.StopStaleSweeper();
  return 0;
}
