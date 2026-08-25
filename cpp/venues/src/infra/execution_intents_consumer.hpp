#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "cex/common/kafka.hpp"
#include "app/execution_planning_uc.hpp"

namespace cex::matching::infra {

class ExecutionIntentsConsumer {
 public:
  ExecutionIntentsConsumer(app::IExecutionPlanningUseCases* uc,
                           const std::string& brokers);

  void start();
  void stop();

 private:
  void loop();

  app::IExecutionPlanningUseCases* uc_;
  std::string brokers_;
  std::atomic<bool> running_{false};
  std::thread t_;
};

}  // namespace cex::matching::infra