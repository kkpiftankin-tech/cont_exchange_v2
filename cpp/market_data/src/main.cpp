#include <grpcpp/grpcpp.h>
#include <algorithm>

#include "cex/common/defer.hpp"
#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/market_data_uc.hpp"
#include "infra/clickhouse/clickhouse_liquidity_curve_storage.hpp"
#include "infra/clickhouse/order_book_storage.hpp"
#include "infra/clickhouse_storage.hpp"
#include "infra/kafka_consumer.hpp"
#include "infra/liquidity_curve_memory_storage.hpp"
#include "infra/order_book_channel.hpp"
#include "transport/grpc_market_data_service.hpp"

int main() {
  const std::string listen_addr =
      cex::common::Env::get_string("MARKET_DATA_GRPC_LISTEN", "0.0.0.0:50054");

  const std::string brokers = cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  cex::market_data::infra::ClickHouseConfig ch_cfg;
  ch_cfg.host = cex::common::Env::get_string("CLICKHOUSE_HOST", "clickhouse");
  ch_cfg.port = cex::common::Env::get_int("CLICKHOUSE_PORT", 8123);
  ch_cfg.tcp_port = cex::common::Env::get_int("CLICKHOUSE_TCP_PORT", 9000);
  ch_cfg.database = cex::common::Env::get_string("CLICKHOUSE_DB", "default");
  ch_cfg.batchresults_table =
      cex::common::Env::get_string("CLICKHOUSE_BATCHRESULTS_TABLE", "batchresults");
  ch_cfg.fills_table = cex::common::Env::get_string("CLICKHOUSE_FILLS_TABLE", "fills");
  ch_cfg.execution_venue_table =
      cex::common::Env::get_string("CLICKHOUSE_EXECUTION_VENUE_TABLE", "execution_venue");
  ch_cfg.marketdata_table =
      cex::common::Env::get_string("CLICKHOUSE_MARKETDATA_TABLE", "marketdata");
  ch_cfg.liquidity_curves_table =
      cex::common::Env::get_string("CLICKHOUSE_LIQUIDITY_CURVES_TABLE", "liquidity_curves");
  ch_cfg.liquidity_curves_retention_days = std::max(
      1, cex::common::Env::get_int("CLICKHOUSE_LIQUIDITY_CURVES_RETENTION_DAYS", 90));
  ch_cfg.user = cex::common::Env::get_string("CLICKHOUSE_USER", "default");
  ch_cfg.password = cex::common::Env::get_string("CLICKHOUSE_PASSWORD", "");
  ch_cfg.timeout_ms = cex::common::Env::get_int("CLICKHOUSE_TIMEOUT_MS", 3000);

  cex::market_data::infra::ClickHouseBatchStorage clickhouse_storage(ch_cfg);
  if (!clickhouse_storage.EnsureSchema()) {
    cex::common::log_json("WARN", "ClickHouse schema initialization failed");
  }

  cex::market_data::infra::clickhouse::OrderBookStorage clickhouse_storage2(ch_cfg);

  // Create ClickHouse storage for liquidity curves
  cex::market_data::infra::clickhouse::ClickHouseLiquidityCurveStorage ch_curve_storage(ch_cfg);
  ch_curve_storage.EnsureSchema();

  cex::market_data::infra::OrderBookChannel channel;
  auto _ = cex::common::Defer([&channel] { channel.Close(); });

  // Create in-memory liquidity curve storage for fast access
  cex::market_data::infra::LiquidityCurveMemoryStorage memory_curve_storage;

  cex::market_data::app::MarketDataUseCases uc(&clickhouse_storage,
                                                &clickhouse_storage,
                                                &clickhouse_storage2, 
                                                &channel, 
                                                &memory_curve_storage,
                                                &ch_curve_storage);

  cex::market_data::infra::MarketDataKafkaConsumer consumer(&uc, brokers);
  consumer.start();

  cex::market_data::transport::GrpcMarketDataService svc(&uc, &channel);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  auto server = builder.BuildAndStart();
  cex::common::log_json("INFO", "MarketData gRPC listening", {{"addr", listen_addr}});
  server->Wait();

  consumer.stop();
  return 0;
}
