#include "infra/kafka_consumers.hpp"

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

#include "fob/ledger/v1/ledger.pb.h"
#include "fob/matching/v1/batch_outputs.pb.h"

namespace cex::ledger::infra {

KafkaConsumers::KafkaConsumers(app::LedgerUseCases* uc,
                               const std::string& brokers)
    : uc_(uc), brokers_(brokers) {}

void KafkaConsumers::start() {
  running_.store(true);
  t1_ = std::thread([this] { loop_batch_outputs(); });
  t2_ = std::thread([this] { loop_execution_intents(); });
  t3_ = std::thread([this] { loop_execution_reports(); });
  t4_ = std::thread([this] { loop_execution_groups(); });
}

void KafkaConsumers::stop() {
  running_.store(false);
  if (t1_.joinable()) t1_.join();
  if (t2_.joinable()) t2_.join();
  if (t3_.joinable()) t3_.join();
  if (t4_.joinable()) t4_.join();
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
