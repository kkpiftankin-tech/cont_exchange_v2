#include "venue_liquidity_curve.hpp"

#include <chrono>

namespace cex::observability::infra::mappers {

namespace detail {

domain::Timestamp FromProto(const google::protobuf::Timestamp& t) {
  const auto duration =
      std::chrono::seconds{t.seconds()} + std::chrono::nanoseconds{t.nanos()};
  return domain::Timestamp{
      std::chrono::duration_cast<domain::Timestamp::duration>(duration)};
}

}  // namespace detail

domain::VenueLiquidityCurve FromProto(const fob::venue::v1::VenueLiquidityCurve& proto) {
  domain::VenueLiquidityCurve result;
  result.venue = proto.venue_id();
  result.snapshot = proto.snapshot_id();
  result.confidence = proto.confidence();
  result.timestamp = detail::FromProto(proto.timestamp());
  return result;
}

}  // namespace cex::observability::infra::mappers
