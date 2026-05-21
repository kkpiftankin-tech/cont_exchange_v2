#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/amm_pool_extractor.hpp"
#include "domain/venue_adapter.hpp"

namespace cex::venues::domain {

// Configuration for virtual LOB synthesis from AMM state.
struct VirtualLobConfig {
  // Number of price levels per side.
  std::size_t levels_per_side{20};

  // Scale for output price/qty decimals.
  int32_t price_scale{8};
  int32_t qty_scale{8};

  // Pool fee rate (e.g. 0.003 for 30bps Uniswap tier).
  double fee_rate{0.003};

  // Uniswap v3 tick spacing (1, 10, 60, 200 depending on fee tier).
  int32_t tick_spacing{60};
};

// Virtual order book synthesized from AMM pool state.
struct VirtualLob {
  std::vector<VenueBookLevel> bids;
  std::vector<VenueBookLevel> asks;

  double mid_price{0.0};

  bool empty() const { return bids.empty() || asks.empty(); }
};

// F11-AMM-2: Build virtual LOB from AMM concentrated-liquidity state.
//
// Two strategies:
//  1. Tick-based (preferred): uses initialized tick levels and liquidity to
//     compute real price/quantity at each tick range. This produces an
//     accurate representation of the actual liquidity distribution.
//  2. Uniform fallback: when tick data is unavailable, distributes liquidity
//     uniformly around mid price (same approach as the existing adapter).
//
// The output feeds directly into the standard LOB→FOB canonicalizer pipeline.
VirtualLob BuildVirtualLOBFromAMM(const AmmPoolExtractResult& pool,
                                  const VirtualLobConfig& config);

// Build one side of the virtual book from tick-level liquidity data.
// For bids: walks ticks downward from current tick.
// For asks: walks ticks upward from current tick.
std::vector<VenueBookLevel> BuildTickBasedSide(
    const VenuePoolState& pool_state,
    double mid_price,
    double fee_rate,
    int32_t price_scale,
    int32_t qty_scale,
    std::size_t max_levels,
    bool is_bid);

// Build one side using uniform distribution around mid price.
// Fallback when tick data is not available.
std::vector<VenueBookLevel> BuildUniformSide(
    double mid_price,
    double fee_rate,
    double base_liquidity,
    int32_t price_scale,
    int32_t qty_scale,
    std::size_t levels,
    bool is_bid);

}  // namespace cex::venues::domain
