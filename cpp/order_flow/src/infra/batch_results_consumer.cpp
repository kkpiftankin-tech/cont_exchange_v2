#include "infra/batch_results_consumer.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "fob/matching/v1/batch.pb.h"

namespace cex::order_flow::infra {

namespace {
constexpr int kPollTimeoutMs = 500;
const char* kTopic = "batch.outputs";
const char* kGroupId = "order_flow_batch_outputs";
const char* kClientId = "order_flow";
}  // namespace

BatchResultsConsumer::BatchResultsConsumer(app::OrderFlowUseCases* uc,
                                           const std::string& brokers)
    : uc_(uc) {
  cex::common::KafkaConsumerConfig cfg;
  cfg.brokers = brokers;
  cfg.group_id = kGroupId;
  cfg.client_id = kClientId;
  cfg.enable_auto_commit = false;
  cfg.auto_offset_reset = "latest";  // only react to live fills, not history
  consumer_ = std::make_unique<cex::common::KafkaConsumer>(cfg);
}

BatchResultsConsumer::~BatchResultsConsumer() { Stop(); }

void BatchResultsConsumer::Start() {
  if (running_.exchange(true)) return;  // already running

  if (!consumer_->subscribe({kTopic})) {
    cex::common::log_json("ERROR",
                          "OrderFlow batch.outputs consumer failed to subscribe",
                          {{"topic", kTopic}});
    running_ = false;
    return;
  }
  cex::common::log_json("INFO",
                        "OrderFlow batch.outputs consumer subscribed",
                        {{"topic", kTopic},
                         {"group_id", kGroupId}});

  thread_ = std::thread([this] { Loop(); });
}

void BatchResultsConsumer::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) {
    thread_.join();
  }
}

void BatchResultsConsumer::Loop() {
  while (running_.load()) {
    const bool ok = consumer_->poll_once(
        kPollTimeoutMs,
        [this](const std::string& topic,
               const std::string& key,
               const std::string& payload) {
          (void)key;
          if (topic != kTopic) return;
          fob::matching::v1::BatchResult batch;
          if (!cex::common::from_bytes(payload, batch)) {
            cex::common::log_json("ERROR",
                                  "OrderFlow failed to parse BatchResult");
            return;
          }
          if (uc_ != nullptr) {
            uc_->ApplyBatchResult(batch);
          }
        });
    if (!ok) {
      // Fatal consumer error — log and exit loop. Service should be restarted.
      cex::common::log_json("ERROR",
                            "OrderFlow batch.outputs consumer fatal error",
                            {{"topic", kTopic}});
      break;
    }
  }
}

}  // namespace cex::order_flow::infra
