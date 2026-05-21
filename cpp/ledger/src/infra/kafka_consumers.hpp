#pragma once
#include <atomic>
#include <thread>

#include "cex/common/kafka.hpp"
#include "app/ledger_uc.hpp"

namespace cex::ledger::infra {

// Starts background Kafka consumers for:
// - batch.outputs -> ApplyBatchResult
// - execution.intents -> RememberExecutionIntent
// - execution.reports / execution.venue -> ApplyExecutionReport
class KafkaConsumers {
 public:
  KafkaConsumers(app::LedgerUseCases* uc,
                 const std::string& brokers);

  void start();
  void stop();

 private:
  void loop_batch_outputs();
  void loop_execution_intents();
  void loop_execution_reports();

  app::LedgerUseCases* uc_;
  std::string brokers_;

  std::atomic<bool> running_{false};
  std::thread t1_;
  std::thread t2_;
  std::thread t3_;
};

}  // namespace cex::ledger::infra
