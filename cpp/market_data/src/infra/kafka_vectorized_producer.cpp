// ============================================================================
// kafka_vectorized_producer.cpp — F-05A (T-F05A-205). См. заголовок в .hpp.
// ============================================================================

#include "infra/kafka_vectorized_producer.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"

namespace cex::market_data::infra {

KafkaVectorizedProducer::KafkaVectorizedProducer(const std::string& brokers,
                                                 const std::string& topic)
    : topic_(topic),
      producer_(cex::common::KafkaConfig{.brokers = brokers,
                                         .client_id = "market_data_vectorized"}) {}

void KafkaVectorizedProducer::Publish(
    const fob::marketdata::v1::VectorizedLiquiditySnapshot& snapshot) {
  const std::string payload = cex::common::to_bytes(snapshot);
  const std::string& key = snapshot.batch_id();
  if (!producer_.produce(topic_, key, payload)) {
    cex::common::log_json("WARN", "Failed to produce marketdata.vectorized",
                          {{"service", "market_data"},
                           {"topic", "marketdata.vectorized"},
                           {"batch_id", key}});
  }
}

}  // namespace cex::market_data::infra
