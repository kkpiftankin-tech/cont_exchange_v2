#pragma once
// ============================================================================
// kafka_ce_clearing_producer.hpp — F-05A CE (вариant A). market_data infra.
//
// Публикует CeClearingInput в Kafka ce.clearing.input. Partition key = batch_id.
// Паттерн как у KafkaVectorizedProducer.
// ============================================================================

#include <string>

#include "app/ports/i_ce_clearing_publisher.hpp"
#include "cex/common/kafka.hpp"

namespace cex::market_data::infra {

class KafkaCeClearingProducer final : public app::ICeClearingPublisher {
 public:
  explicit KafkaCeClearingProducer(const std::string& brokers,
                                   const std::string& topic = "ce.clearing.input");

  void Publish(const fob::marketdata::v1::CeClearingInput& input) override;

 private:
  std::string topic_;
  cex::common::KafkaProducer producer_;
};

}  // namespace cex::market_data::infra
