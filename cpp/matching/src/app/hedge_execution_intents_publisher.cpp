#include "app/hedge_execution_intents_publisher.hpp"

#include <unordered_set>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"
#include "infra/kafka/execution_intents_producer.hpp"

namespace cex::matching::app {

HedgeExecutionIntentsPublishResult PublishAutoHedgeExecutionIntents(
    const std::string& batch_id,
    const std::vector<fob::execution::v1::ExecutionIntent>& intents,
    infra::ExecutionIntentsProducer& producer) {
  HedgeExecutionIntentsPublishResult result;
  result.attempted = intents.size();

  std::unordered_set<std::string> seen_hedge_flow_ids;
  seen_hedge_flow_ids.reserve(intents.size());
  for (const auto& intent : intents) {
    const std::string& hedge_flow_id = intent.hedge_flow_id();
    if (!hedge_flow_id.empty()) {
      const auto [_, inserted] = seen_hedge_flow_ids.insert(hedge_flow_id);
      if (!inserted) {
        ++result.deduped;
        cex::common::log_json(
            "WARN",
            "Skipped duplicate auto-hedge execution intent in batch",
            {{"batchId", batch_id},
             {"providerId", intent.provider_id()},
             {"symbol", intent.instrument().symbol()},
             {"targetQty",
              intent.has_target_qty()
                  ? cex::common::Decimal::from_proto(intent.target_qty())
                        .to_string()
                  : "0"},
             {"urgency", std::to_string(static_cast<int>(intent.urgency()))},
             {"hedgeFlowId", hedge_flow_id},
             {"topic", "execution.intents"}});
        continue;
      }
    }

    if (!producer.produce_auto_hedge(intent)) {
      result.success = false;
      continue;
    }
    ++result.published;
  }

  return result;
}

}  // namespace cex::matching::app
