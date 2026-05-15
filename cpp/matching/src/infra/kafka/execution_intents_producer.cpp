#include "execution_intents_producer.hpp"

#include <unordered_set>
#include <utility>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"

namespace cex::matching::infra {

ExecutionIntentsProducer::ExecutionIntentsProducer(
    cex::common::KafkaProducer& producer)
    : producer_(&producer) {}

ExecutionIntentsProducer::ExecutionIntentsProducer(ProduceFn produce_fn)
    : produce_fn_(std::move(produce_fn)) {}

namespace {

std::string DefaultExecutionIntentKey(
    const fob::execution::v1::ExecutionIntent& intent) {
  return intent.meta().partition_key().empty()
             ? (intent.intent_id().empty() ? intent.batch_id()
                                           : intent.intent_id())
             : intent.meta().partition_key();
}

}  // namespace

bool ExecutionIntentsProducer::produce(
    const fob::execution::v1::ExecutionIntent& intent) {
  const std::string key = DefaultExecutionIntentKey(intent);
  const std::string payload = cex::common::to_bytes(intent);
  const bool ok = ProduceRecord("execution.intents", key, payload);
  cex::common::log_json(ok ? "INFO" : "ERROR", "Published execution intent",
                        {{"service", "matching"},
                         {"component", "execution_planning"},
                         {"participant", "Execution Planning"},
                         {"stage", "publish_execution_intent"},
                         {"topic", "execution.intents"},
                         {"intent_id", intent.intent_id()},
                         {"batch_id", intent.batch_id()},
                         {"order_id", intent.internal_order_id()},
                         {"venue", intent.venue()},
                         {"symbol", intent.instrument().symbol()},
                         {"side", std::to_string(static_cast<int>(intent.side()))},
                         {"target_qty",
                          intent.has_target_qty()
                              ? cex::common::Decimal::from_proto(intent.target_qty()).to_string()
                              : "0"},
                         {"limit_price",
                          intent.has_limit_price()
                              ? cex::common::Decimal::from_proto(intent.limit_price()).to_string()
                              : "0"},
                         {"venue_symbol", intent.venue_symbol()},
                         {"payload_bytes", std::to_string(payload.size())},
                         {"source_file",
                          "cpp/matching/src/infra/kafka/execution_intents_producer.cpp"}});
  return ok;
}

bool ExecutionIntentsProducer::produce(
    const std::vector<fob::execution::v1::ExecutionIntent>& intents) {
  bool ok = true;
  for (const auto& intent : intents) {
    ok = produce(intent) && ok;
  }

  cex::common::log_json("INFO", "Published execution intents",
                        {{"count", std::to_string(intents.size())},
                         {"success", ok ? "true" : "false"}});
  return ok;
}

bool ExecutionIntentsProducer::produce_auto_hedge(
    const fob::execution::v1::ExecutionIntent& intent) {
  const std::string key =
      intent.hedge_flow_id().empty() ? DefaultExecutionIntentKey(intent)
                                     : intent.hedge_flow_id();
  const std::string payload = cex::common::to_bytes(intent);
  const bool ok = ProduceRecord("execution.intents", key, payload);
  cex::common::log_json(
      ok ? "INFO" : "ERROR",
      "Published auto-hedge execution intent",
      {{"service", "matching"},
       {"component", "execution_planning"},
       {"participant", "Execution Planning"},
       {"stage", "publish_auto_hedge_execution_intent"},
       {"topic", "execution.intents"},
       {"batchId", intent.batch_id()},
       {"providerId", intent.provider_id()},
       {"symbol", intent.instrument().symbol()},
       {"targetQty",
        intent.has_target_qty()
            ? cex::common::Decimal::from_proto(intent.target_qty()).to_string()
            : "0"},
       {"urgency", std::to_string(static_cast<int>(intent.urgency()))},
       {"hedgeFlowId", intent.hedge_flow_id()},
       {"payload_bytes", std::to_string(payload.size())},
       {"source_file",
        "cpp/matching/src/infra/kafka/execution_intents_producer.cpp"}});
  return ok;
}

bool ExecutionIntentsProducer::produce_auto_hedge(
    const std::vector<fob::execution::v1::ExecutionIntent>& intents) {
  bool ok = true;
  std::size_t deduped = 0;
  std::unordered_set<std::string> seen_hedge_flow_ids;
  seen_hedge_flow_ids.reserve(intents.size());
  for (const auto& intent : intents) {
    if (!intent.hedge_flow_id().empty()) {
      const auto [_, inserted] =
          seen_hedge_flow_ids.insert(intent.hedge_flow_id());
      if (!inserted) {
        ++deduped;
        cex::common::log_json(
            "WARN",
            "Skipped duplicate auto-hedge execution intent in batch vector",
            {{"batchId", intent.batch_id()},
             {"providerId", intent.provider_id()},
             {"symbol", intent.instrument().symbol()},
             {"hedgeFlowId", intent.hedge_flow_id()},
             {"topic", "execution.intents"}});
        continue;
      }
    }
    ok = produce_auto_hedge(intent) && ok;
  }

  cex::common::log_json(
      "INFO",
      "Published auto-hedge execution intents",
      {{"count", std::to_string(intents.size())},
       {"deduped", std::to_string(deduped)},
       {"success", ok ? "true" : "false"}});
  return ok;
}

bool ExecutionIntentsProducer::ProduceRecord(const std::string& topic,
                                             const std::string& key,
                                             const std::string& payload) {
  if (produce_fn_) return produce_fn_(topic, key, payload);
  if (producer_ == nullptr) return false;
  return producer_->produce(topic, key, payload);
}

}  // namespace cex::matching::infra
