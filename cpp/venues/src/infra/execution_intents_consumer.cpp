#include "infra/execution_intents_consumer.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "fob/execution/v1/execution.pb.h"

namespace cex::matching::infra {

ExecutionIntentsConsumer::ExecutionIntentsConsumer(
    app::IExecutionPlanningUseCases* uc,
    const std::string& brokers)
    : uc_(uc), brokers_(brokers) {}

void ExecutionIntentsConsumer::start() {
  if (running_.exchange(true)) return;
  t_ = std::thread([this] { loop(); });
}

void ExecutionIntentsConsumer::stop() {
  running_.store(false);
  if (t_.joinable()) t_.join();
}

void ExecutionIntentsConsumer::loop() {
  cex::common::KafkaConsumer consumer({
      .brokers = brokers_,
      .group_id = "execution-planning",
      .client_id = "matching-execution-planner",
      .enable_auto_commit = false, // Для обеспечения idempotency/exactly-once вручную
  });

  if (!consumer.subscribe({"execution.intents"})) {
    cex::common::log_json("ERROR", "Failed to subscribe to execution.intents");
    return;
  }

  cex::common::log_json("INFO", "Execution Intents consumer started",
                        {{"topic", "execution.intents"},
                         {"group_id", "execution-planning",}});

  while (running_.load()) {
    bool ok = consumer.poll_once(500, [this](const std::string& topic,
                                             const std::string& key,
                                             const std::string& payload) {
      fob::execution::v1::ExecutionIntent intent;
      
      // 1. Валидация схемы (десериализация)
      if (!cex::common::from_bytes(payload, intent)) {
        cex::common::log_json("ERROR", "Failed to parse ExecutionIntent", 
                              {{"key", key}});
        return;
      }

      // 2. Логирование входящего интента
      cex::common::log_json("INFO", "Received execution intent",
                            {{"intent_id", intent.intent_id()},
                             {"hedge_flow_id", intent.hedge_flow_id()},
                             {"symbol", intent.instrument().symbol()},
                             {"side", std::to_string(static_cast<int>(intent.side()))}});

      // 3. Передача в UseCase (там должна быть проверка idempotency по hedge_flow_id)
      uc_->OnExecutionIntent(intent);
    });

    if (!ok) {
      cex::common::log_json("ERROR", "Kafka consumer poll error");
      break;
    }
  }

  cex::common::log_json("INFO", "Execution Intents consumer stopped");
}

}  // namespace cex::matching::infra