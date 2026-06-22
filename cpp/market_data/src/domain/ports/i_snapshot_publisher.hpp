#pragma once

#include "domain/entities/market_data_snapshot.hpp"

namespace cex::market_data::domain {

// Публикация MarketDataSnapshot во внешние системы (Kafka, WebSocket).
class ISnapshotPublisher {
 public:
  virtual ~ISnapshotPublisher() = default;
  virtual void PublishSnapshot(const MarketDataSnapshot& snap) = 0;
};

// Публикация risk alert при аномальном спреде.
class IRiskAlertPublisher {
 public:
  virtual ~IRiskAlertPublisher() = default;
  virtual void PublishSpreadAlert(const std::string& asset,
                                  const common::Decimal& spread_bps,
                                  const common::Decimal& threshold_bps) = 0;
};

}  // namespace cex::market_data::domain
