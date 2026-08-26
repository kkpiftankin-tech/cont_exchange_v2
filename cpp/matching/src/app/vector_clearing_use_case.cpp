// ============================================================================
// vector_clearing_use_case.cpp — F-05A (T-F05A-305, safe slice). См. .hpp.
// ============================================================================

#include "app/vector_clearing_use_case.hpp"

#include "cex/common/decimal.hpp"

namespace cex::matching::app {

namespace {
double ToDouble(const fob::common::v1::Decimal& d) {
  return static_cast<double>(cex::common::Decimal::from_proto(d));
}
}  // namespace

std::vector<domain::VectorSegment> VectorClearingUseCase::MapSegments(
    const fob::marketdata::v1::VectorClearingInput& input) {
  std::vector<domain::VectorSegment> out;
  out.reserve(static_cast<std::size_t>(input.segments_size()));
  for (const auto& ps : input.segments()) {
    domain::VectorSegment seg;
    seg.segment_id = ps.segment_id();
    seg.source_order_id = ps.source_order_id();
    seg.w.reserve(static_cast<std::size_t>(ps.w_size()));
    for (const auto& wj : ps.w()) {
      seg.w.push_back(ToDouble(wj));
    }
    seg.d_hl = ToDouble(ps.d_hl());
    seg.q_max = ToDouble(ps.q_max());
    out.push_back(std::move(seg));
  }
  return out;
}

VectorClearingOutcome VectorClearingUseCase::Clear(
    const fob::marketdata::v1::VectorClearingInput& input) const {
  VectorClearingOutcome outcome;
  const std::vector<domain::VectorSegment> segments = MapSegments(input);
  const int num_assets = input.basis().num_assets();

  outcome.num_assets = num_assets;
  outcome.num_segments = static_cast<int>(segments.size());

  outcome.solve = solver_.Solve(segments, num_assets);
  outcome.surplus = domain::DecideSurplus(outcome.solve.residual,
                                          outcome.solve.residual_norm, policy_,
                                          residual_tolerance_, decimal_scale_);
  return outcome;
}

}  // namespace cex::matching::app
