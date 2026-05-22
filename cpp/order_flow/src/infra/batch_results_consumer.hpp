#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "app/order_flow_uc.hpp"
#include "cex/common/kafka.hpp"

namespace cex::order_flow::infra {

// Kafka consumer for topic `batch.outputs`. Deserialises BatchResult
// messages and feeds them into OrderFlowUseCases::ApplyBatchResult so the
// in-memory FlowOrder store stays in sync with matching's fills.
//
// Runs on a single background thread. Stop() must be called before the
// embedded use-case is destroyed.
class BatchResultsConsumer {
 public:
  BatchResultsConsumer(app::OrderFlowUseCases* uc,
                       const std::string& brokers);
  ~BatchResultsConsumer();

  BatchResultsConsumer(const BatchResultsConsumer&) = delete;
  BatchResultsConsumer& operator=(const BatchResultsConsumer&) = delete;

  void Start();
  void Stop();

 private:
  void Loop();

  app::OrderFlowUseCases* uc_{nullptr};
  std::unique_ptr<cex::common::KafkaConsumer> consumer_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace cex::order_flow::infra
