// ============================================================================
// vectorized_liquidity.cpp — F-05A (T-F05A-205). См. заголовок в .hpp.
// ============================================================================

#include "transport/mappers/vectorized_liquidity.hpp"

#include <cmath>  // std::isfinite, std::llround, std::pow

namespace cex::market_data::transport {

namespace mv1 = fob::marketdata::v1;

namespace {

cex::common::Decimal QuantizeW(double value, std::int32_t scale) {
  if (!std::isfinite(value)) return cex::common::Decimal{0, scale};
  const double factor = std::pow(10.0, static_cast<double>(scale));
  return cex::common::Decimal{static_cast<std::int64_t>(std::llround(value * factor)),
                              scale};
}

mv1::VectorLevelSide MapSide(domain::LevelSide side) {
  return side == domain::LevelSide::kBid ? mv1::VECTOR_LEVEL_SIDE_BID
                                         : mv1::VECTOR_LEVEL_SIDE_ASK;
}

void SetTs(google::protobuf::Timestamp* ts, std::int64_t ms) {
  if (ms <= 0) return;
  ts->set_seconds(ms / 1000);
  ts->set_nanos(static_cast<std::int32_t>((ms % 1000) * 1000000));
}

}  // namespace

mv1::VectorizedLiquiditySnapshot ToVectorizedSnapshot(
    const domain::VectorizeResult& result, const std::string& batch_id,
    std::int64_t event_ts_ms, std::int32_t decimal_scale) {
  mv1::VectorizedLiquiditySnapshot snap;
  snap.set_batch_id(batch_id);

  mv1::VectorClearingInput* input = snap.mutable_input();
  input->set_batch_id(batch_id);

  // AssetBasis (детерминированный basis_id = активы через '-').
  mv1::AssetBasis* basis = input->mutable_basis();
  basis->set_num_assets(result.basis.num_assets);
  std::string basis_id;
  for (std::size_t i = 0; i < result.basis.assets.size(); ++i) {
    mv1::AssetBasis::Entry* e = basis->add_assets();
    e->set_index(static_cast<std::int32_t>(i));
    e->set_asset(result.basis.assets[i]);
    if (!basis_id.empty()) basis_id += "-";
    basis_id += result.basis.assets[i];
  }
  basis->set_basis_id(basis_id);
  SetTs(basis->mutable_created_at(), event_ts_ms);

  // Сегменты (столбцы W).
  for (const auto& s : result.segments) {
    mv1::VectorFlowSegment* ps = input->add_segments();
    ps->set_segment_id(s.segment_id);
    ps->set_source_order_id(s.source_order_id);
    ps->set_venue_id(s.venue_id);
    ps->set_pair(s.pair);
    ps->set_side(MapSide(s.side));
    for (double wj : s.w) {
      *ps->add_w() = QuantizeW(wj, decimal_scale).to_proto();
    }
    *ps->mutable_p_low() = s.p_low.to_proto();
    *ps->mutable_p_high() = s.p_high.to_proto();
    *ps->mutable_d_hl() = s.d_hl.to_proto();
    *ps->mutable_q_rate() = s.q_rate.to_proto();
    *ps->mutable_q_max() = s.q_max.to_proto();
    *ps->mutable_remaining_quantity() = s.q_max.to_proto();
    SetTs(ps->mutable_source_timestamp(), s.source_timestamp_ms);
  }

  return snap;
}

}  // namespace cex::market_data::transport
