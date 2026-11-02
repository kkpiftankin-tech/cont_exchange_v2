// ============================================================================
// kafka_ce_clearing_producer.cpp — F-05A CE. См. заголовок в .hpp.
// ============================================================================

#include "infra/kafka_ce_clearing_producer.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"

namespace cex::market_data::infra {

KafkaCeClearingProducer::KafkaCeClearingProducer(const std::string& brokers,
                                                 const std::string& topic)
    : topic_(topic),
      producer_(cex::common::KafkaConfig{.brokers = brokers,
                                         .client_id = "market_data_ce_clearing"}) {}

void KafkaCeClearingProducer::Publish(
    const fob::marketdata::v1::CeClearingInput& input) {
  const std::string payload = cex::common::to_bytes(input);
  const std::string& key = input.batch_id();
  if (!producer_.produce(topic_, key, payload)) {
    cex::common::log_json("WARN", "Failed to produce ce.clearing.input",
                          {{"service", "market_data"},
                           {"topic", "ce.clearing.input"},
                           {"batch_id", key}});
  }
}

}  // namespace cex::market_data::infra
