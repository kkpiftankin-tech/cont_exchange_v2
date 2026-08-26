#pragma once
// ============================================================================
// kafka_vectorized_producer.hpp — F-05A (T-F05A-205). market_data infra.
//
// Публикует VectorizedLiquiditySnapshot в Kafka marketdata.vectorized.
// Partition key = batch_id (см. create_topics.sh). Паттерн как у
// KafkaSnapshotsProducer.
// ============================================================================

#include <string>

#include "app/ports/i_vectorized_publisher.hpp"
#include "cex/common/kafka.hpp"

namespace cex::market_data::infra {

class KafkaVectorizedProducer final : public app::IVectorizedPublisher {
 public:
  explicit KafkaVectorizedProducer(const std::string& brokers,
                                   const std::string& topic = "marketdata.vectorized");

  void Publish(
      const fob::marketdata::v1::VectorizedLiquiditySnapshot& snapshot) override;

 private:
  std::string topic_;
  cex::common::KafkaProducer producer_;
};

}  // namespace cex::market_data::infra
