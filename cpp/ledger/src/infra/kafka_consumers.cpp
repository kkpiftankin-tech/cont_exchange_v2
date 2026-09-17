#include "infra/kafka_consumers.hpp"

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

#include "fob/ledger/v1/ledger.pb.h"
#include "fob/matching/v1/batch_outputs.pb.h"
#include "fob/treasury/v1/treasury.pb.h"  // F-18 §11: CePositionDeltaBatch

#include <utility>
#include <vector>

namespace cex::ledger::infra {

namespace {

// F-18 v2 (T-F18-201/202, ADR-061 §1): AssetDelta.agent_kind → строка,
// как хранится в agent_positions_/PG ce_agent_position (docs/07-data/
// ce-agent-position.md). AGENT_KIND_UNSPECIFIED → "" (kind ещё неизвестен —
// не перетирает ранее известный kind в LedgerUseCases::ApplyPositionDelta).
std::string AgentKindToString(fob::treasury::v1::AgentKind kind) {
  switch (kind) {
    case fob::treasury::v1::AGENT_KIND_TRANSLATOR: return "translator";
    case fob::treasury::v1::AGENT_KIND_ARBITRAGEUR: return "arbitrageur";
    default: return "";
  }
}

}  // namespace

KafkaConsumers::KafkaConsumers(app::LedgerUseCases* uc,
                               const std::string& brokers)
    : uc_(uc), brokers_(brokers) {}

void KafkaConsumers::start() {
  running_.store(true);
  t1_ = std::thread([this] { loop_batch_outputs(); });
  t2_ = std::thread([this] { loop_execution_intents(); });
  t3_ = std::thread([this] { loop_execution_reports(); });
  t4_ = std::thread([this] { loop_execution_groups(); });
  t5_ = std::thread([this] { loop_ce_position_delta(); });  // F-18 §11
}

void KafkaConsumers::stop() {
  running_.store(false);
  if (t1_.joinable()) t1_.join();
  if (t2_.joinable()) t2_.join();
  if (t3_.joinable()) t3_.join();
  if (t4_.joinable()) t4_.join();
  if (t5_.joinable()) t5_.join();
}

// F-18 §11 (variant A): consume ce.position.delta → ApplyPositionDelta (накопление
// позиции биржи от вектор-клиринга + снапшот старая→Δ→новая по batch_id).
void KafkaConsumers::loop_ce_position_delta() {
  cex::common::KafkaConsumer consumer({
      .brokers = brokers_,
      .group_id = "ledger-ce-pos-delta",
      .client_id = "ledger",
      .enable_auto_commit = false,
  });
  consumer.subscribe({"ce.position.delta"});
  while (running_.load()) {
    bool ok = consumer.poll_once(500, [this](const std::string& topic,
                                             const std::string& key,
                                             const std::string& payload) {
      (void)topic; (void)key;
      fob::treasury::v1::CePositionDeltaBatch dpb;
      if (!cex::common::from_bytes(payload, dpb)) {
        cex::common::log_json("ERROR", "Failed to parse CePositionDeltaBatch");
        return;
      }
      // T-F18-201/202 (ADR-061 §1/§7, CE_AGENT_POS): маршрутизация data-driven
      // по каждой AssetDelta-записи, а не отдельным env-флагом здесь — сам
      // факт непустого agent_id уже означает, что matching эмитировал эту
      // запись с CE_AGENT_POS=1. agent_id пуст → LEGACY per-asset путь
      // (node_deltas, ce_committed_) БЕЗ ИЗМЕНЕНИЙ — обратная совместимость.
      std::vector<std::pair<std::string, cex::common::Decimal>> node_deltas;
      std::vector<cex::ledger::app::LedgerUseCases::AgentDelta> agent_deltas;
      for (const auto& ad : dpb.deltas()) {
        if (!ad.agent_id().empty()) {
          cex::ledger::app::LedgerUseCases::AgentDelta d;
          d.agent_id = ad.agent_id();
          d.agent_kind = AgentKindToString(ad.agent_kind());
          d.asset = ad.asset();
          d.venue = ad.venue();
          d.delta = cex::common::Decimal::from_proto(ad.delta());
          d.price_used = cex::common::Decimal::from_proto(ad.price_used());  // F-18 v2 Э3
          agent_deltas.push_back(std::move(d));
        } else {
          node_deltas.emplace_back(ad.asset(), cex::common::Decimal::from_proto(ad.delta()));
        }
      }
      uc_->ApplyPositionDelta(dpb.batch_id(), dpb.event_time_ms(), node_deltas, agent_deltas);
    });
    if (!ok) break;
  }
}

// F-09 (T-F09-060): consume execution.groups → ApplyExecutionGroup (grouped postings).
void KafkaConsumers::loop_execution_groups() {
  cex::common::KafkaConsumer consumer({
      .brokers = brokers_,
      .group_id = "ledger-execution-groups",
      .client_id = "ledger",
      .enable_auto_commit = false,
  });
  consumer.subscribe({"execution.groups"});

  while (running_.load()) {
    bool ok = consumer.poll_once(
        500, [this](const std::string& topic, const std::string& key,
                    const std::string& payload) {
          (void)topic;
          (void)key;
          fob::matching::v1::ExecutionGroup eg;
          if (!cex::common::from_bytes(payload, eg)) {
            cex::common::log_json("ERROR", "Failed to parse execution.groups payload");
            return;
          }
          uc_->ApplyExecutionGroup(eg);
        });
    if (!ok) break;
  }
}

void KafkaConsumers::loop_batch_outputs() {
  cex::common::KafkaConsumer consumer({
      .brokers=brokers_,
      .group_id="ledger-batch",
      .client_id="ledger",
      .enable_auto_commit=false,
  });
  consumer.subscribe({"batch.outputs"});

  while (running_.load()) {
    bool ok = consumer.poll_once(500, [this](const std::string& topic,
                                           const std::string& key,
                                           const std::string& payload) {
      (void)topic; (void)key;
      fob::matching::v1::BatchResult batch;
      fob::matching::v1::BatchOutputs out;
      if (cex::common::from_bytes(payload, out)) {
        batch = out.result();
      } else if (!cex::common::from_bytes(payload, batch)) {
        cex::common::log_json(
            "ERROR", "Failed to parse batch.outputs payload as BatchOutputs/BatchResult");
        return;
      }

      fob::ledger::v1::ApplyBatchResultRequest req;
      auto* meta = req.mutable_meta();
      meta->set_event_id(cex::common::uuid_v4());
      *meta->mutable_ts_event() = cex::common::now_ts();
      meta->set_source("ledger");
      meta->set_correlation_id(batch.meta().correlation_id());

      *req.mutable_batch() = batch;

      uc_->ApplyBatchResult(req);
      cex::common::log_json("INFO", "Ledger applied batch",
                            {{"service", "ledger"},
                             {"component", "ledger_consumer"},
                             {"participant", "Settlement & Ledger"},
                             {"stage", "consume_batch_result"},
                             {"topic", "batch.outputs"},
                             {"batch_id", batch.batch_id()},
                             {"fills", std::to_string(batch.fills_size())},
                             {"clear_prices", std::to_string(batch.clear_prices_size())},
                             {"executed_rates", std::to_string(batch.executed_rates_size())},
                             {"source_file",
                              "cpp/ledger/src/infra/kafka_consumers.cpp"}});
    });

    if (!ok) break;
  }
}

void KafkaConsumers::loop_execution_reports() {
  cex::common::KafkaConsumer consumer({
      .brokers=brokers_,
      .group_id="ledger-exec",
      .client_id="ledger",
      .enable_auto_commit=false,
  });
  consumer.subscribe({"execution.reports", "execution.venue"});

  while (running_.load()) {
    bool ok = consumer.poll_once(500, [this](const std::string& topic,
                                           const std::string& key,
                                           const std::string& payload) {
      (void)key;
      fob::execution::v1::ExecutionReport report;
      if (!cex::common::from_bytes(payload, report)) {
        cex::common::log_json("ERROR", "Failed to parse ExecutionReport");
        return;
      }

      fob::ledger::v1::ApplyExecutionReportRequest req;
      auto* meta = req.mutable_meta();
      meta->set_event_id(cex::common::uuid_v4());
      *meta->mutable_ts_event() = cex::common::now_ts();
      meta->set_source("ledger");
      meta->set_correlation_id(report.meta().correlation_id());

      *req.mutable_report() = report;
      uc_->ApplyExecutionReport(req);
      cex::common::log_json("INFO", "Ledger applied execution report",
                            {{"service", "ledger"},
                             {"component", "ledger_consumer"},
                             {"participant", "Settlement & Ledger"},
                             {"stage", "consume_execution_report"},
                             {"topic", topic},
                             {"intent_id", report.intent_id()},
                             {"report_id", report.report_id()},
                             {"venue", report.venue()},
                             {"symbol", report.instrument().symbol()},
                             {"status", std::to_string(report.status())},
                             {"filled_qty",
                              report.has_filled_qty()
                                  ? cex::common::Decimal::from_proto(report.filled_qty()).to_string()
                                  : "0"},
                             {"remaining_qty",
                              report.has_remaining_qty()
                                  ? cex::common::Decimal::from_proto(report.remaining_qty()).to_string()
                                  : "0"},
                             {"average_price",
                              report.has_average_price()
                                  ? cex::common::Decimal::from_proto(report.average_price()).to_string()
                                  : "0"},
                             {"source_file",
                              "cpp/ledger/src/infra/kafka_consumers.cpp"}});
    });

    if (!ok) break;
  }
}

void KafkaConsumers::loop_execution_intents() {
  cex::common::KafkaConsumer consumer({
      .brokers=brokers_,
      .group_id="ledger-intents",
      .client_id="ledger",
      .enable_auto_commit=false,
  });
  consumer.subscribe({"execution.intents"});

  while (running_.load()) {
    bool ok = consumer.poll_once(500, [this](const std::string& topic,
                                           const std::string& key,
                                           const std::string& payload) {
      (void)topic;
      (void)key;
      fob::execution::v1::ExecutionIntent intent;
      if (!cex::common::from_bytes(payload, intent)) {
        cex::common::log_json("ERROR", "Failed to parse ExecutionIntent");
        return;
      }

      uc_->RememberExecutionIntent(intent);
      cex::common::log_json("INFO", "Ledger stored execution intent",
                            {{"service", "ledger"},
                             {"component", "ledger_consumer"},
                             {"participant", "Settlement & Ledger"},
                             {"stage", "consume_execution_intent"},
                             {"topic", "execution.intents"},
                             {"intent_id", intent.intent_id()},
                             {"batch_id", intent.batch_id()},
                             {"venue", intent.venue()},
                             {"symbol", intent.instrument().symbol()},
                             {"target_qty",
                              intent.has_target_qty()
                                  ? cex::common::Decimal::from_proto(intent.target_qty()).to_string()
                                  : "0"},
                             {"limit_price",
                              intent.has_limit_price()
                                  ? cex::common::Decimal::from_proto(intent.limit_price()).to_string()
                                  : "0"},
                             {"source_file",
                              "cpp/ledger/src/infra/kafka_consumers.cpp"}});
    });

    if (!ok) break;
  }
}

}  // namespace cex::ledger::infra
