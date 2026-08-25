#include "infra/kafka_consumer.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "fob/matching/v1/batch.pb.h"
#include "fob/matching/v1/batch_outputs.pb.h"

namespace cex::backtest::infra {

BacktestKafkaConsumer::BacktestKafkaConsumer(app::BacktestUseCases* uc,
                                             const std::string& brokers)
    : uc_(uc), brokers_(brokers) {}

void BacktestKafkaConsumer::start() {
  running_.store(true);
  t_ = std::thread([this] { loop(); });
}

void BacktestKafkaConsumer::stop() {
  running_.store(false);
  if (t_.joinable()) t_.join();
}

void BacktestKafkaConsumer::loop() {
  cex::common::KafkaConsumer consumer({
      .brokers = brokers_,
      .group_id = "backtest-batch",
      .client_id = "backtest",
      .enable_auto_commit = false,
  });
  consumer.subscribe({"batch.outputs"});

  cex::common::log_json("INFO", "Backtest Kafka consumer started",
                        {{"topic", "batch.outputs"},
                         {"group_id", "backtest-batch"}});

  while (running_.load()) {
    bool ok = consumer.poll_once(500, [this](const std::string& topic,
                                             const std::string& key,
                                             const std::string& payload) {
      (void)key;
      (void)topic;
      fob::matching::v1::BatchResult batch;
      fob::matching::v1::BatchOutputs out;
      if (cex::common::from_bytes(payload, out)) {
        batch = out.result();
      } else if (!cex::common::from_bytes(payload, batch)) {
        cex::common::log_json("ERROR",
                              "Backtest: failed to parse batch.outputs as BatchOutputs/BatchResult");
        return;
      }
      uc_->OnBatchResult(batch);
    });

    if (!ok) break;
  }

  cex::common::log_json("INFO", "Backtest Kafka consumer stopped");
}

}  // namespace cex::backtest::infra
