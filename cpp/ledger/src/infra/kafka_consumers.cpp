#include "infra/kafka_consumers.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

#include "fob/ledger/v1/ledger.pb.h"

namespace cex::ledger::infra {

KafkaConsumers::KafkaConsumers(app::LedgerUseCases* uc,
                               const std::string& brokers)
    : uc_(uc), brokers_(brokers) {}

void KafkaConsumers::start() {
  running_.store(true);
  t1_ = std::thread([this] { loop_batch_outputs(); });
  t2_ = std::thread([this] { loop_execution_reports(); });
}

void KafkaConsumers::stop() {
  running_.store(false);
  if (t1_.joinable()) t1_.join();
  if (t2_.joinable()) t2_.join();
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
      if (!cex::common::from_bytes(payload, batch)) {
        cex::common::log_json("ERROR", "Failed to parse BatchResult");
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
      cex::common::log_json("INFO", "Ledger applied batch", {{"batch_id", batch.batch_id()}});
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
  consumer.subscribe({"execution.reports"});

  while (running_.load()) {
    bool ok = consumer.poll_once(500, [this](const std::string& topic,
                                           const std::string& key,
                                           const std::string& payload) {
      (void)topic; (void)key;
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
    });

    if (!ok) break;
  }
}

}  // namespace cex::ledger::infra
