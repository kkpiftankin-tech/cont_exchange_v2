#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/venue_adapter.hpp"

namespace cex::venues::domain {

// Configuration for AMM pool state extraction.
struct AmmPoolExtractorConfig {
  // Default scale for price and quantity values.
  int32_t price_scale{8};
  int32_t qty_scale{8};

  // Maximum number of tick levels to retain.
  std::size_t max_tick_levels{256};
};

// Result of a single pool state extraction attempt.
struct AmmPoolExtractResult {
  bool valid{false};

  VenuePoolState pool_state;

  // Reserves (for constant-product / hybrid AMMs).
  cex::common::Decimal reserve_base{0, 0};
  cex::common::Decimal reserve_quote{0, 0};

  // Derived mid price from sqrtPriceX96 / tick / reserves.
  double mid_price{0.0};

  // Block-level sequence for dedup.
  uint64_t sequence{0};

  // Validation issues (empty if valid).
  std::string error;
};

// F11-AMM-1: Pure domain extractor for Uniswap v3 / concentrated-liquidity
// pool state.
//
// Responsibilities:
//  - Parse sqrtPriceX96, tick, liquidity, initialized ticks from a flat
//    key-value map (adapter-agnostic; the infra layer converts JSON → map).
//  - Validate required fields, monotonic block numbers, non-zero liquidity.
//  - Compute mid price from sqrtPriceX96 (preferred), tick (fallback),
//    or reserves (last resort).
//  - Cache last valid state per pool and reject stale/duplicate updates.
class AmmPoolExtractor {
 public:
  explicit AmmPoolExtractor(const AmmPoolExtractorConfig& config);

  // --- Core extraction ---

  // Extract pool state from a flat field map.
  // Keys expected (Uniswap v3 naming, snake_case accepted):
  //   pool_address / poolAddress
  //   sqrt_price_x96 / sqrtPriceX96
  //   tick
  //   liquidity
  //   block_number / blockNumber
  //   finalized / isFinalized
  //   reserve_base / reserve0 / baseReserve
  //   reserve_quote / reserve1 / quoteReserve
  //   ticks (vector of {tick, liquidity_net})
  AmmPoolExtractResult Extract(
      const std::unordered_map<std::string, std::string>& fields,
      const std::vector<VenuePoolTickLevel>& tick_levels) const;

  // --- Caching ---

  // Apply an extracted result to the cache. Returns false if the update is
  // stale (block_number <= cached block_number for the same pool).
  bool ApplyToCache(const std::string& pool_key,
                    const AmmPoolExtractResult& result);

  // Get the last cached result for a pool. Returns nullopt if no valid
  // state has been cached.
  std::optional<AmmPoolExtractResult> GetCached(
      const std::string& pool_key) const;

  // Clear all cached state.
  void ClearCache();

  // --- Price computation (static, testable) ---

  // Compute mid price from sqrtPriceX96 string (Uniswap v3 Q64.96 format).
  // Returns 0.0 if the string is empty or invalid.
  static double MidPriceFromSqrtPriceX96(const std::string& sqrt_price_x96);

  // Compute mid price from tick index: price = 1.0001^tick.
  // Returns 0.0 if tick == 0 or result is not finite.
  static double MidPriceFromTick(int64_t tick);

  // Compute mid price from reserves: price = quote / base.
  // Returns 0.0 if either reserve is non-positive.
  static double MidPriceFromReserves(const cex::common::Decimal& reserve_base,
                                     const cex::common::Decimal& reserve_quote);

  // Resolve mid price using the priority chain:
  //  sqrtPriceX96 > tick > reserves.
  static double ResolveMidPrice(const VenuePoolState& pool_state,
                                const cex::common::Decimal& reserve_base,
                                const cex::common::Decimal& reserve_quote);

 private:
  AmmPoolExtractorConfig config_;
  std::unordered_map<std::string, AmmPoolExtractResult> cache_;
};

}  // namespace cex::venues::domain
