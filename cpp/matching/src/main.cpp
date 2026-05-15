#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "app/matching_loop.hpp"
#include "app/solver_metrics.hpp"
#include "domain/flow_order_repository.hpp"
#include "domain/solver_impl.hpp"
#include "infra/market_data/market_data_client.hpp"
#include "infra/postgres/postgres_flow_order_repository.hpp"
#include "infra/postgres/postgres_solver_config_repository.hpp"
#include "transport/grpc_isolation_matching_service.hpp"
#include "crow.h"

int main() {
  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  const int interval_ms =
      cex::common::Env::get_int("BATCH_INTERVAL_MS", 5000);
  const auto metrics_port = static_cast<uint16_t>(
      cex::common::Env::get_int("MATCHING_METRICS_PORT", 8081));

  std::shared_ptr<cex::matching::domain::IFlowOrderRepository> flow_order_repository;
  std::unique_ptr<cex::matching::domain::SolverConfigRepositoryPort> solver_config_repo;
  std::shared_ptr<cex::matching::infra::MarketDataClient> market_data_client;

  const std::string market_data_target =
      cex::common::Env::get_string("MARKET_DATA_GRPC_TARGET", "market_data:50054");
  const std::string market_data_venue =
      cex::common::Env::get_string("MARKET_DATA_REFERENCE_VENUE", "binance");
  market_data_client = std::make_shared<cex::matching::infra::MarketDataClient>(
      market_data_target, market_data_venue);

  const auto postgres_dsn = cex::common::Env::try_get_string("MATCHING_POSTGRES_DSN");
  if (postgres_dsn.has_value() && !postgres_dsn->empty()) {
    flow_order_repository =
        std::make_shared<cex::matching::infra::PostgresFlowOrderRepository>(*postgres_dsn);
    solver_config_repo = 
        std::make_unique<cex::matching::infra::PostgresSolverConfigRepository>(*postgres_dsn);
    cex::common::log_json("INFO", "Matching Postgres repository enabled",
                          {{"env", "MATCHING_POSTGRES_DSN"}});
  } else {
    cex::common::log_json("INFO", "Matching Postgres repository disabled; using in-memory active orders");
  }

  cex::common::log_json("INFO", "Matching starting",
                        {{"brokers", brokers},
                         {"interval_ms", std::to_string(interval_ms)},
                         {"metrics_port", std::to_string(metrics_port)}});

  cex::matching::app::SolverMetrics metrics;
  cex::matching::app::MatchingLoop loop(brokers, interval_ms,
    flow_order_repository, std::move(solver_config_repo), market_data_client, metrics);
  loop.start();

  const std::string isolation_listen =
      cex::common::Env::get_string("MATCHING_ISOLATION_GRPC_LISTEN", "0.0.0.0:50053");
  cex::matching::domain::ContinuousClearingSolver isolation_solver;
  cex::matching::transport::GrpcIsolationSolverService isolation_service(&isolation_solver);
  grpc::ServerBuilder isolation_builder;
  isolation_builder.AddListeningPort(isolation_listen, grpc::InsecureServerCredentials());
  isolation_builder.RegisterService(&isolation_service);
  auto isolation_server = isolation_builder.BuildAndStart();
  if (isolation_server) {
    cex::common::log_json("INFO", "Matching isolation gRPC listening",
                          {{"addr", isolation_listen}});
  } else {
    cex::common::log_json("ERROR", "Matching isolation gRPC failed to start",
                          {{"addr", isolation_listen}});
  }

  crow::SimpleApp metrics_app;
  CROW_ROUTE(metrics_app, "/healthz")([] { return crow::response(200, "ok"); });
  CROW_ROUTE(metrics_app, "/metrics")([&metrics] {
    crow::response response;
    response.code = 200;
    response.set_header("Content-Type",
                        "text/plain; version=0.0.4; charset=utf-8");
    response.write(metrics.RenderPrometheus());
    return response;
  });

  CROW_ROUTE(metrics_app, "/orders/<string>")(
      [&loop](const std::string& order_id) {
        const auto snap = loop.snapshot_order(order_id);
        crow::response response;
        response.set_header("Content-Type", "application/json");
        if (!snap.has_value()) {
          response.code = 404;
          response.write("{\"error\":\"not_found\"}");
          return response;
        }
        response.code = 200;
        std::ostringstream body;
        body << "{\"order_id\":\"" << snap->order_id << "\","
             << "\"status\":\"" << snap->status << "\","
             << "\"total_qty\":" << snap->total_qty << ","
             << "\"filled_qty\":" << snap->filled_qty << "}";
        response.write(body.str());
        return response;
      });

  [[maybe_unused]] auto metrics_server =
      metrics_app.port(metrics_port).run_async();

  // run forever
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
  }
  return 0;
}
