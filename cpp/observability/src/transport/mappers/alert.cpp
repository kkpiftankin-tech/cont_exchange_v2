#include "alert.hpp"

#include <google/protobuf/util/time_util.h>

namespace cex::observability::transport::mappers {

namespace detail {

fob::observability::v1::Severity ToProto(domain::Severity severity) {
  switch (severity) {
    case domain::Severity::Info:
      return fob::observability::v1::SEVERITY_INFO;
    case domain::Severity::Warning:
      return fob::observability::v1::SEVERITY_WARNING;
    case domain::Severity::Error:
      return fob::observability::v1::SEVERITY_ERROR;
    case domain::Severity::Critical:
      return fob::observability::v1::SEVERITY_CRITICAL;
    default:
      throw std::invalid_argument("unknown Severity");
  }
}

fob::observability::v1::AlertCode ToProto(domain::AlertCode code) {
  switch (code) {
    case domain::AlertCode::LowVenueHealthScore:
      return fob::observability::v1::ALERT_CODE_LOW_VENUE_HEALTH_SCORE;
    case domain::AlertCode::CircuitBreakerOpen:
      return fob::observability::v1::ALERT_CODE_CIRCUIT_BREAKER_OPEN;
    case domain::AlertCode::LowLiquidityCurveConfidence:
      return fob::observability::v1::ALERT_CODE_LOW_LIQUIDITY_CURVE_CONFIDENCE;
    default:
      throw std::invalid_argument("unknown AlertCode");
  }
}

fob::observability::v1::AlertStatus ToProto(domain::AlertStatus status) {
  switch (status) {
    case domain::AlertStatus::Firing:
      return fob::observability::v1::ALERT_STATUS_FIRING;
    case domain::AlertStatus::Resolved:
      return fob::observability::v1::ALERT_STATUS_RESOLVED;
    default:
      throw std::invalid_argument("unknown AlertStatus");
  }
}

google::protobuf::Timestamp ToProto(domain::Timestamp ts) {
  auto nanos =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(ts).time_since_epoch().count();
  return google::protobuf::util::TimeUtil::NanosecondsToTimestamp(nanos);
}

}  // namespace detail

fob::observability::v1::Alert ToProto(const domain::Alert &alert) {
  fob::observability::v1::Alert result;
  result.set_id(alert.id);
  result.set_code(detail::ToProto(alert.code));
  result.set_severity(detail::ToProto(alert.severity));
  result.set_status(detail::ToProto(alert.status));
  *result.mutable_timestamp() = detail::ToProto(domain::Timestamp::clock::now());
  return result;
}

}  // namespace cex::observability::transport::mappers
