// ============================================================================
// curve_to_levels.cpp — F-05A (T-F05A-205). См. заголовок в .hpp.
// ============================================================================

#include "app/curve_to_levels.hpp"

#include <algorithm>  // std::min
#include <cmath>      // std::isfinite, std::llround, std::pow
#include <string>

namespace cex::market_data::app {

namespace {

cex::common::Decimal Quantize(double value, std::int32_t scale) {
  if (!std::isfinite(value)) return cex::common::Decimal{0, scale};
  const double factor = std::pow(10.0, static_cast<double>(scale));
  return cex::common::Decimal{static_cast<std::int64_t>(std::llround(value * factor)),
                              scale};
}

std::int64_t TsToMs(const google::protobuf::Timestamp& ts) {
  return ts.seconds() * 1000 + ts.nanos() / 1000000;
}

/// Разложить одну сторону кривой в уровни. side задаёт знак/семантику.
void AppendSide(const fob::venue::v1::SideLiquidityCurve& side_curve,
                domain::LevelSide side, const fob::venue::v1::VenueLiquidityCurve& curve,
                std::int32_t scale, std::vector<domain::ExternalOrderLevel>& out) {
  const int n =
      std::min(side_curve.q_grid_size(), side_curve.p_of_q_size());
  double prev_q = 0.0;
  for (int k = 0; k < n; ++k) {
    const double q_cum = side_curve.q_grid(k);
    const double price = side_curve.p_of_q(k);
    const double step_qty = q_cum - prev_q;
    prev_q = q_cum;
    if (!std::isfinite(step_qty) || step_qty <= 0.0) continue;
    if (!std::isfinite(price) || price <= 0.0) continue;

    domain::ExternalOrderLevel lvl;
    lvl.venue_id = curve.venue_id();
    lvl.pair = curve.instrument().symbol();
    lvl.base_asset = curve.instrument().base();
    lvl.quote_asset = curve.instrument().quote();
    lvl.side = side;
    lvl.source_order_id = curve.venue_id() + "|" + lvl.pair + "|" +
                          domain::ToString(side) + "|" + std::to_string(k);
    lvl.price = Quantize(price, scale);
    lvl.quantity = Quantize(step_qty, scale);
    lvl.remaining_quantity = lvl.quantity;
    lvl.ts_event_ms = TsToMs(curve.timestamp());
    out.push_back(std::move(lvl));
  }
}

}  // namespace

std::vector<domain::ExternalOrderLevel> LevelsFromCurve(
    const fob::venue::v1::VenueLiquidityCurve& curve, std::int32_t decimal_scale) {
  std::vector<domain::ExternalOrderLevel> levels;
  if (curve.has_bid_curve()) {
    AppendSide(curve.bid_curve(), domain::LevelSide::kBid, curve, decimal_scale, levels);
  }
  if (curve.has_ask_curve()) {
    AppendSide(curve.ask_curve(), domain::LevelSide::kAsk, curve, decimal_scale, levels);
  }
  return levels;
}

}  // namespace cex::market_data::app
