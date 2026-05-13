#pragma once
// =============================================================================
// RiskAlertsPublisher — публикатор сообщений RiskAlert в топик risk.alerts.
// Простая обёртка над KafkaProducer с фиксированным топиком и стратегией
// формирования ключа партиционирования.
// =============================================================================

#include "cex/common/kafka.hpp"
#include "fob/risk/v1/risk.pb.h"

namespace cex::risk::infra {

class RiskAlertsPublisher {
 public:
  explicit RiskAlertsPublisher(cex::common::KafkaProducer producer);

  // Сериализует alert и публикует в risk.alerts. true — успешно положили в очередь.
  bool publish(const fob::risk::v1::RiskAlert& alert);

 private:
  cex::common::KafkaProducer producer_;
};

}  // namespace cex::risk::infra
