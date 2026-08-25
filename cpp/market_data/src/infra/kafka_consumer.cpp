#include "fob/matching/v1/batch.pb.h"
#include "fob/matching/v1/batch_outputs.pb.h"
#include "fob/matching/v1/fill_event.pb.h"
#include "fob/matching/v1/execution_group.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "fob/orders/v1/orders.pb.h"
#include "fob/venue/v1/venue.pb.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"

#include "infra/kafka_consumer.hpp"

namespace cex::market_data::infra {

MarketDataKafkaConsumer::MarketDataKafkaConsumer(app::MarketDataUseCases* uc,
                                                 const std::string& brokers)
    : uc_(uc), brokers_(brokers) {}

void MarketDataKafkaConsumer::start() {
  running_.store(true);
  t_ = std::thread([this] { loop(); });
}

void MarketDataKafkaConsumer::stop() {
  running_.store(false);
  if (t_.joinable()) t_.join();
}

void MarketDataKafkaConsumer::loop() {
  const bool trace_each_message =
      cex::common::Env::get_string("MARKET_DATA_TRACE_EACH_MESSAGE", "0") == "1";
  auto last_progress_log = std::chrono::steady_clock::now();

  while (running_.load()) {
    cex::common::KafkaConsumer consumer({
        .brokers = brokers_,
        .group_id = "marketdata",
        .client_id = "market_data",
        // High-throughput load path: avoid synchronous per-message commit bottleneck.
        .enable_auto_commit = true,
        .auto_offset_reset = "latest",
        .max_poll_interval_ms = 1800000,
    });
    // F-11 integration path: Market Data Service consumes raw market data,
    // normalized snapshots and FOB curves.
    if (!consumer.subscribe(
            {"marketdata.raw", "batch.outputs", "fills",
             "orders.normalized",   // F5-3: FlowOrder для FOB aggregate curves
             "venue.liquidity.fob", "venue.snapshots", "execution.venue",
             "execution.groups"})) {   // F-09: grouped combo execution
      cex::common::log_json("ERROR", "MarketData Kafka subscribe failed; retrying");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    cex::common::log_json("INFO", "MarketData Kafka subscribed",
                          {{"topics", "marketdata.raw,batch.outputs,venue.liquidity.fob,venue.snapshots,execution.venue,execution.groups"},
                           {"group_id", "marketdata"},
                           {"auto_commit", "true"}});

    while (running_.load()) {
      bool ok = consumer.poll_once(
          500, [this, trace_each_message](const std::string& topic, const std::string& key, const std::string& payload) {
            (void)key;
            try {
              if (topic == "marketdata.raw") {
                fob::marketdata::v1::MarketDataRaw evt;
                if (!cex::common::from_bytes(payload, evt)) {
                  cex::common::log_json("ERROR", "Failed to parse MarketDataRaw");
                  return;
                }
                uc_->OnMarketDataRaw(evt);
                return;
              }

              if (topic == "batch.outputs") {
                fob::matching::v1::BatchResult batch;
                fob::matching::v1::BatchOutputs out;
                if (cex::common::from_bytes(payload, out)) {
                  batch = out.result();
                } else if (!cex::common::from_bytes(payload, batch)) {
                  cex::common::log_json(
                      "ERROR", "Failed to parse batch.outputs payload as BatchOutputs/BatchResult");
                  return;
                }
                uc_->OnBatchResult(batch);
                return;
              }

              // F5-3: FlowOrder events для FOB aggregate curves
              if (topic == "orders.normalized") {
                fob::orders::v1::OrdersNormalized msg;
                if (!cex::common::from_bytes(payload, msg)) {
                  cex::common::log_json("ERROR", "Failed to parse OrdersNormalized");
                  return;
                }
                if (msg.has_create()) {
                  uc_->OnOrderCreate(msg.create().order());
                } else if (msg.has_cancel()) {
                  // FlowOrderCancel не содержит instrument — используем RemoveOrderById
                  uc_->OnOrderCancel(msg.cancel().order_id(), "");
                }
                return;
              }

              // F-05: per-fill events для расчёта effective spread.
              // Топик `fills` несёт FillEvent (а НЕ FlowFill) — адаптируем к
              // FlowFill, который ожидают OnFillEvent/ComputeEffectiveSpread.
              if (topic == "fills") {
                fob::matching::v1::FillEvent ev;
                if (!cex::common::from_bytes(payload, ev)) {
                  cex::common::log_json("ERROR", "Failed to parse FillEvent from fills topic");
                  return;
                }
                fob::matching::v1::FlowFill fill;
                fill.set_order_id(ev.order_id());
                if (ev.asset_legs_size() > 0) {
                  *fill.mutable_instrument() = ev.asset_legs(0);
                  // Нормализуем символ ("BTC/USDT" → "BTCUSDT"), иначе GetCurrentMid
                  // не найдёт mid в кэше (он keyed нормализованным asset).
                  std::string sym = fill.instrument().symbol();
                  sym.erase(std::remove(sym.begin(), sym.end(), '/'), sym.end());
                  fill.mutable_instrument()->set_symbol(sym);
                }
                if (ev.has_exec_qty())   *fill.mutable_executed_qty() = ev.exec_qty();
                if (ev.has_exec_price()) *fill.mutable_price()        = ev.exec_price();
                const auto ts = std::chrono::system_clock::now();
                const std::string fid = !ev.fill_id().empty() ? ev.fill_id() : key;
                uc_->OnFillEvent(fill, fid, ev.batch_id(), ts);
                return;
              }

              // F-09 observability: grouped combo execution → ClickHouse grouped_*.
              if (topic == "execution.groups") {
                fob::matching::v1::ExecutionGroup eg;
                if (!cex::common::from_bytes(payload, eg)) {
                  cex::common::log_json("ERROR", "Failed to parse execution.groups ExecutionGroup");
                  return;
                }
                uc_->OnExecutionGroup(eg);
                return;
              }

              if (topic == "venue.liquidity.fob") {
                fob::venue::v1::VenueLiquidityCurve curve;
                if (!cex::common::from_bytes(payload, curve)) {
                  cex::common::log_json("ERROR", "Failed to parse VenueLiquidityCurve");
                  return;
                }

                cex::common::log_json(trace_each_message ? "DEBUG" : "INFO",
                                      "MarketData received venue.liquidity.fob",
                                      {{"service", "market_data"},
                                       {"component", "market_data_consumer"},
                                       {"participant", "Market Data Service"},
                                       {"stage", "consume_curve"},
                                       {"topic", "venue.liquidity.fob"},
                                       {"venue", curve.venue_id()},
                                       {"symbol", curve.instrument().symbol()},
                                       {"confidence", std::to_string(curve.confidence())},
                                       {"level", curve.level()},
                                       {"source_file",
                                        "cpp/market_data/src/infra/kafka_consumer.cpp"}});

                uc_->OnLiquidityCurve(curve);
                return;
              }
              if (topic == "venue.snapshots") {
                fob::venue::v1::VenueSnapshot snapshot;
                if (!cex::common::from_bytes(payload, snapshot)) {
                  cex::common::log_json("ERROR", "Failed to parse VenueSnapshot");
                  return;
                }
                cex::common::log_json(trace_each_message ? "DEBUG" : "INFO",
                                      "MarketData received venue.snapshot",
                                      {{"service", "market_data"},
                                       {"component", "market_data_consumer"},
                                       {"participant", "Market Data Service"},
                                       {"stage", "consume_snapshot"},
                                       {"topic", "venue.snapshots"},
                                       {"venue", snapshot.venue_id()},
                                       {"symbol", snapshot.instrument().symbol()},
                                       {"status", snapshot.status()},
                                       {"source_file",
                                        "cpp/market_data/src/infra/kafka_consumer.cpp"}});
                uc_->OnVenueSnapshot(snapshot);
                return;
              }

              if (topic == "execution.venue") {
                fob::execution::v1::ExecutionReport report;
                if (!cex::common::from_bytes(payload, report)) {
                  cex::common::log_json("ERROR", "Failed to parse ExecutionReport");
                  return;
                }
                cex::common::log_json(trace_each_message ? "DEBUG" : "INFO",
                                      "MarketData received execution.venue",
                                      {{"service", "market_data"},
                                       {"component", "market_data_consumer"},
                                       {"participant", "Market Data Service"},
                                       {"stage", "consume_execution_report"},
                                       {"topic", "execution.venue"},
                                       {"venue", report.venue()},
                                       {"intent_id", report.intent_id()},
                                       {"status", std::to_string(report.status())},
                                       {"source_file",
                                        "cpp/market_data/src/infra/kafka_consumer.cpp"}});
                uc_->OnExecutionReport(report);
                return;
              }

              cex::common::log_json("WARN", "KafkaConsumer:: got message from unknown topic",
                                    {{"topic", topic}});
            } catch (const std::exception& e) {
              cex::common::log_json("ERROR", "MarketData consumer handler failed",
                                    {{"topic", topic}, {"error", e.what()}});
            } catch (...) {
              cex::common::log_json("ERROR", "MarketData consumer handler failed",
                                    {{"topic", topic}, {"error", "unknown_exception"}});
            }
          });

      if (ok) {
        // Best-effort periodic progress diagnostics for CI dumps.
        if (trace_each_message) {
          // Already logged per-message above.
        }
      }
      if (!ok) {
        cex::common::log_json("WARN", "MarketData poll_once failed; recreating consumer");
        break;
      }

      // Lightweight periodic liveness marker even without per-message tracing.
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - last_progress_log).count();
      if (elapsed_ms >= 5000) {
        cex::common::log_json("INFO", "MarketData consumer loop alive",
                              {{"poll_timeout_ms", "500"}});
        last_progress_log = now;
      }
    }

    if (running_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }
}

}  // namespace cex::market_data::infra
