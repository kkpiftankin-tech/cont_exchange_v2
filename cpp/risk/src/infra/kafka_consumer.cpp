#include "kafka_consumer.hpp"

#include "cex/common/kafka.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "fob/execution/v1/execution.pb.h"
#include "fob/venue/v1/venue.pb.h"

namespace cex::risk::infra {

namespace {

double MaxCurveQty(const fob::venue::v1::SideLiquidityCurve& curve) {
  if (curve.q_grid_size() <= 0) return 0.0;
  return curve.q_grid(curve.q_grid_size() - 1);
}

}  // namespace

KafkaConsumer::KafkaConsumer(std::string brokers, app::RiskUseCases& uc)
    : brokers_(brokers),
      uc_(uc),
      venue_liquidity_fob_consumer_({
          .brokers = brokers_,
          .group_id = "risk",
          .client_id = "venue_liquidity_fob",
          .enable_auto_commit = false,
      }),
      venue_health_consumer_({
          .brokers = brokers_,
          .group_id = "risk",
          .client_id = "venue_health",
          .enable_auto_commit = false,
      }),
      execution_reports_consumer_({
          .brokers = brokers_,
          .group_id = "risk",
          .client_id = "execution_venue",
          .enable_auto_commit = false,
      }),
      synthetic_orders_consumer_({
          .brokers = brokers_,
          .group_id = "risk",
          .client_id = "synthetic_orders",
          .enable_auto_commit = false,
      }) {}

void KafkaConsumer::start() {
  venue_liquidity_fob_consumer_.subscribe({"venue.liquidity.fob"});
  venue_health_consumer_.subscribe({"venue.health"});
  execution_reports_consumer_.subscribe({"execution.venue", "execution.reports"});
  synthetic_orders_consumer_.subscribe({"venue.synthetic"});

  running_.store(true);
  venue_liquidity_fob_t_ =
      std::thread([this]() { venue_liquidity_fob_loop(); });
  venue_health_t_ = std::thread([this]() { venue_health_loop(); });
  execution_reports_t_ = std::thread([this]() { execution_reports_loop(); });
  synthetic_orders_t_ = std::thread([this]() { synthetic_orders_loop(); });
}

void KafkaConsumer::stop() {
  // TODO: no method unsubscribe? WTF?
  running_.store(false);
  venue_liquidity_fob_t_.join();
  venue_health_t_.join();
  execution_reports_t_.join();
  synthetic_orders_t_.join();
}

void KafkaConsumer::venue_liquidity_fob_loop() {
  const int timeout_ms = 500;

  auto handler = [this](const std::string&,
                        const std::string&,
                        const std::string& payload) {
    fob::venue::v1::VenueLiquidityCurve curve;
    if (!common::from_bytes(payload, curve)) {
      common::log_json("ERROR", "Failed to parse VenueLiquidityCurve");
      return;
    }

    common::log_json("INFO", "Risk consumed venue.liquidity.fob",
                     {{"service", "risk"},
                      {"component", "risk_consumer"},
                      {"participant", "Risk Manager"},
                      {"stage", "consume_curve"},
                      {"topic", "venue.liquidity.fob"},
                      {"venue", curve.venue_id()},
                      {"symbol", curve.instrument().symbol()},
                      {"level", curve.level()},
                      {"confidence", std::to_string(curve.confidence())},
                      {"bid_levels", std::to_string(curve.bid_curve().q_grid_size())},
                      {"ask_levels", std::to_string(curve.ask_curve().q_grid_size())},
                      {"max_bid_qty", std::to_string(MaxCurveQty(curve.bid_curve()))},
                      {"max_ask_qty", std::to_string(MaxCurveQty(curve.ask_curve()))},
                      {"source_file", "cpp/risk/src/infra/kafka_consumer.cpp"}});
    uc_.CurveChecks(curve);
  };

  while (running_.load()) {
    if (bool ok = venue_liquidity_fob_consumer_.poll_once(timeout_ms, handler);
        !ok) {
      break;
    }
  }
}

void KafkaConsumer::venue_health_loop() {
  const int timeout_ms = 500;

  auto handler = [this](const std::string&,
                        const std::string&,
                        const std::string& payload) {
    fob::venue::v1::VenueHealth health;
    if (!common::from_bytes(payload, health)) {
      common::log_json("ERROR", "Failed to parse VenueHealth");
      return;
    }

    common::log_json("INFO", "Risk consumed venue.health",
                     {{"service", "risk"},
                      {"component", "risk_consumer"},
                      {"participant", "Risk Manager"},
                      {"stage", "consume_health"},
                      {"topic", "venue.health"},
                      {"venue", health.venue()},
                      {"status", std::to_string(static_cast<int>(health.status()))},
                      {"routing",
                       std::to_string(static_cast<int>(health.routing_recommendation()))},
                      {"breaker",
                       std::to_string(static_cast<int>(health.breaker_state()))},
                      {"health_score", std::to_string(health.health_score())},
                      {"source_file", "cpp/risk/src/infra/kafka_consumer.cpp"}});
    uc_.HealthChecks(health);
  };

  while (running_.load()) {
    if (bool ok = venue_health_consumer_.poll_once(timeout_ms, handler); !ok) {
      break;
    }
  }
}

void KafkaConsumer::synthetic_orders_loop() {
  const int timeout_ms = 500;

  auto handler = [this](const std::string&,
                        const std::string&,
                        const std::string& payload) {
    fob::orders::v1::SyntheticFlowOrder order;
    if (!common::from_bytes(payload, order)) {
      common::log_json("ERROR", "Failed to parse SyntheticFlowOrder");
      return;
    }

    common::log_json("INFO", "Risk consumed venue.synthetic",
                     {{"service", "risk"},
                      {"component", "risk_consumer"},
                      {"participant", "Risk Manager"},
                      {"stage", "consume_synthetic_flow_order"},
                      {"topic", "venue.synthetic"},
                      {"venue", order.venue_id()},
                      {"symbol", order.instrument().symbol()},
                      {"side", std::to_string(static_cast<int>(order.side()))},
                      {"q_max_units", std::to_string(order.q_max().units())},
                      {"source_file", "cpp/risk/src/infra/kafka_consumer.cpp"}});
    uc_.OnSyntheticOrder(order);
  };

  while (running_.load()) {
    if (bool ok = synthetic_orders_consumer_.poll_once(timeout_ms, handler);
        !ok) {
      break;
    }
  }
}

void KafkaConsumer::execution_reports_loop() {
  const int timeout_ms = 500;

  auto handler = [this](const std::string&,
                        const std::string&,
                        const std::string& payload) {
    fob::execution::v1::ExecutionReport report;
    if (!common::from_bytes(payload, report)) {
      common::log_json("ERROR", "Failed to parse ExecutionReport");
      return;
    }

    common::log_json("INFO", "Risk consumed execution report",
                     {{"service", "risk"},
                      {"component", "risk_consumer"},
                      {"participant", "Risk Manager"},
                      {"stage", "consume_execution_report"},
                      {"topic", "execution.venue"},
                      {"venue", report.venue()},
                      {"intent_id", report.intent_id()},
                      {"report_id", report.report_id()},
                      {"status", std::to_string(static_cast<int>(report.status()))},
                      {"filled_qty_units", std::to_string(report.filled_qty().units())},
                      {"source_file", "cpp/risk/src/infra/kafka_consumer.cpp"}});
    uc_.OnExecutionReport(report);
  };

  while (running_.load()) {
    if (bool ok = execution_reports_consumer_.poll_once(timeout_ms, handler);
        !ok) {
      break;
    }
  }
}

}  // namespace cex::risk::infra
