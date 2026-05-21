#include "raw_report.hpp"

#include <chrono>

#include <google/protobuf/util/time_util.h>

namespace cex::venue_health::infra::mappers {

namespace detail {

domain::Timestamp FromProto(const ::google::protobuf::Timestamp& t) {
  const auto duration = std::chrono::seconds{t.seconds()} +
                        std::chrono::nanoseconds{t.nanos()};
  return domain::Timestamp{
      std::chrono::duration_cast<domain::Timestamp::duration>(duration)};
}

domain::VenueHealthStatus FromProto(const fob::venue::v1::VenueHealthStatus status) {
  switch (status) {
    case fob::venue::v1::VenueHealthStatus::VENUE_HEALTH_STATUS_UNSPECIFIED:
      return domain::VenueHealthStatus::Unspecified;
    case fob::venue::v1::VenueHealthStatus::VENUE_HEALTH_STATUS_OK:
      return domain::VenueHealthStatus::Ok;
    case fob::venue::v1::VenueHealthStatus::VENUE_HEALTH_STATUS_STALE:
      return domain::VenueHealthStatus::Stale;
    case fob::venue::v1::VenueHealthStatus::VENUE_HEALTH_STATUS_DISCONNECTED:
      return domain::VenueHealthStatus::Disconnected;
    case fob::venue::v1::VenueHealthStatus::VENUE_HEALTH_STATUS_RATE_LIMIT:
      return domain::VenueHealthStatus::RateLimit;
    case fob::venue::v1::VenueHealthStatus::VENUE_HEALTH_STATUS_DEGRADED:
      return domain::VenueHealthStatus::Degraded;
    default:
      throw std::invalid_argument("Unknown VenueHealthStatus");
  }
}

google::protobuf::Timestamp ToProto(const domain::Timestamp& ts) {
  auto nanos =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(ts).time_since_epoch().count();
  return google::protobuf::util::TimeUtil::NanosecondsToTimestamp(nanos);
}

}  // namespace detail

domain::RawReport FromProto(const fob::venue::v1::VenueHealth& raw) {
  domain::RawReport report;
  report.venue = raw.venue();
  report.timestamp = detail::FromProto(raw.timestamp());
  report.venue_status = detail::FromProto(raw.status());
  report.latency = std::chrono::milliseconds(raw.latency_ms());
  report.stale = std::chrono::milliseconds(raw.stale_ms());
  report.error_rate = raw.error_rate();
  report.error_count = raw.error_count();
  report.snapshots_per_sec = raw.snapshots_per_sec();
  report.last_snapshot_age = raw.last_snapshot_age();
  report.reason = raw.reason();
  return report;
}

}  // namespace cex::venue_health::infra::mappers
