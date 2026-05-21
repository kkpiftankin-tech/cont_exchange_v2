#pragma once

#include <cstddef>
#include <cstdint>

#include "cex/common/decimal.hpp"
#include "domain/depth_curve_builder.hpp"
#include "domain/venue_adapter.hpp"

namespace cex::venues::domain {

// F11-AMM-3:
// Direct AMM -> FOB conversion path without intermediate synthetic LOB object.
struct DirectAmmFobConfig {
  // Maximum number of liquidity segments per side.
  std::size_t max_segments_per_side{20};

  // Output scales for price/quantity/cost layers.
  int32_t price_scale{8};
  int32_t qty_scale{8};

  // AMM pool swap fee applied to executable price:
  // buy side  -> p * (1 + fee),
  // sell side -> p * (1 - fee).
  double pool_fee_rate{0.0};

  // Additional execution overhead (routing, approval, bridge, etc.) in bps:
  // buy side  -> p * (1 + overhead),
  // sell side -> p * (1 - overhead).
  double execution_overhead_bps{0.0};

  // Price-impact multiplier for AMM depth shape calibration:
  // p = p0 + (p_raw - p0) * slippage_multiplier.
  double slippage_multiplier{1.0};

  // Fixed quote-cost correction per execution path.
  // Applied to cumulative cost for q>0:
  // buy side  -> S(q) + gas_cost_quote,
  // sell side -> max(0, S(q) - gas_cost_quote).
  double gas_cost_quote{0.0};
};

// Builds FOB tabular curves directly from concentrated AMM pool state.
//
// For side == kBuy:
//   walks ticks upward, producing ask-side execution segments.
// For side == kSell:
//   walks ticks downward, producing bid-side execution segments.
//
// The function returns empty curves when pool state is insufficient.
DepthSideCurves BuildDirectAmmFobCurves(
    const VenuePoolState& pool_state,
    ExecutionSide side,
    const cex::common::Decimal& tau_sec,
    const DirectAmmFobConfig& config = {});

}  // namespace cex::venues::domain
