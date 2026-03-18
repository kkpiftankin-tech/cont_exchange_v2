#pragma once
#include "cex/common/kafka.hpp"
#include "fob/risk/v1/risk.pb.h"

namespace cex::risk::infra {

class RiskAlertsPublisher {
 public:
  explicit RiskAlertsPublisher(cex::common::KafkaProducer producer);

  bool publish(const fob::risk::v1::RiskAlert& alert);

 private:
  cex::common::KafkaProducer producer_;
};

}  // namespace cex::risk::infra
