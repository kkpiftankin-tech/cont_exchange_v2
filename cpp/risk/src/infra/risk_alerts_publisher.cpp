// =============================================================================
// Реализация RiskAlertsPublisher.
// =============================================================================

#include "infra/risk_alerts_publisher.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"

namespace cex::risk::infra {

RiskAlertsPublisher::RiskAlertsPublisher(cex::common::KafkaProducer producer)
    : producer_(std::move(producer)) {}

bool RiskAlertsPublisher::publish(const fob::risk::v1::RiskAlert& alert) {
  const std::string topic = "risk.alerts";
  // Если у алерта есть user_id — партиционируем по нему (тогда события одного
  // пользователя ложатся в одну партицию). Иначе используем alert_id.
  const std::string key = alert.user_id().empty() ? alert.alert_id() : alert.user_id();
  const std::string payload = cex::common::to_bytes(alert);
  bool ok = producer_.produce(topic, key, payload);
  if (ok) {
    cex::common::log_json("INFO", "Published RiskAlert", {{"topic", topic}, {"key", key}});
  }
  return ok;
}

}  // namespace cex::risk::infra
