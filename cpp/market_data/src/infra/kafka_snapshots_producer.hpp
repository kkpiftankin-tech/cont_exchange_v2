#pragma once

#include <string>

#include "domain/ports/i_snapshot_publisher.hpp"
#include "cex/common/kafka.hpp"

namespace cex::market_data::infra {

// Публикует MarketDataSnapshot в Kafka `marketdata.snapshots`.
// Partition key = asset для гарантии порядка обновлений по инструменту.
class KafkaSnapshotsProducer final : public domain::ISnapshotPublisher {
 public:
  explicit KafkaSnapshotsProducer(const std::string& brokers,
                                   const std::string& topic = "marketdata.snapshots");

  void PublishSnapshot(const domain::MarketDataSnapshot& snap) override;

 private:
  std::string topic_;
  cex::common::KafkaProducer producer_;
};

// Публикует risk alert в Kafka `risk.alerts`.
class KafkaRiskAlertPublisher final : public domain::IRiskAlertPublisher {
 public:
  explicit KafkaRiskAlertPublisher(const std::string& brokers,
                                    const std::string& topic = "risk.alerts");

  void PublishSpreadAlert(const std::string& asset,
                          const common::Decimal& spread_bps,
                          const common::Decimal& threshold_bps) override;

 private:
  std::string topic_;
  cex::common::KafkaProducer producer_;
};

}  // namespace cex::market_data::infra
