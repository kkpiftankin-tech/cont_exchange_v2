#include "domain/amm_virtual_lob.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace cex::venues::domain {

namespace {

cex::common::Decimal DecimalFromDouble(double value, int32_t scale) {
  if (!std::isfinite(value)) return cex::common::Decimal{0, scale};
  const double factor = std::pow(10.0, scale);
  const double scaled = value * factor;
  if (std::abs(scaled) > static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return cex::common::Decimal{0, scale};
  }
  return cex::common::Decimal{static_cast<int64_t>(std::llround(scaled)), scale};
}

// Uniswap v3: price at tick = 1.0001^tick
double PriceAtTick(int64_t tick) {
  return std::pow(1.0001, static_cast<double>(tick));
}

// Uniswap v3: liquidity available in a tick range [tick_lower, tick_upper)
// gives quantity of base token = L * (1/sqrt(p_lower) - 1/sqrt(p_upper))
// and quantity of quote token = L * (sqrt(p_upper) - sqrt(p_lower)).
//
// For bids (selling base): we care about base token amount.
// For asks (buying base): we care about base token amount too (what you get).
double BaseAmountInRange(double liquidity, double price_lower,
                         double price_upper) {
  if (liquidity <= 0.0 || price_lower <= 0.0 || price_upper <= 0.0) return 0.0;
  if (price_lower >= price_upper) return 0.0;

  const double sqrt_lower = std::sqrt(price_lower);
  const double sqrt_upper = std::sqrt(price_upper);
  if (sqrt_lower <= 0.0 || sqrt_upper <= 0.0) return 0.0;

  // base amount = L * (1/sqrt(p_lower) - 1/sqrt(p_upper))
  const double amount = liquidity * (1.0 / sqrt_lower - 1.0 / sqrt_upper);
  if (!std::isfinite(amount) || amount <= 0.0) return 0.0;
  return amount;
}

}  // namespace

std::vector<VenueBookLevel> BuildTickBasedSide(
    const VenuePoolState& pool_state,
    double mid_price,
    double fee_rate,
    int32_t price_scale,
    int32_t qty_scale,
    std::size_t max_levels,
    bool is_bid) {
  std::vector<VenueBookLevel> out;
  if (pool_state.ticks.empty() || mid_price <= 0.0 || max_levels == 0) {
    return out;
  }

  out.reserve(max_levels);

  // Parse liquidity as double.
  double current_liquidity = 0.0;
  if (!pool_state.liquidity.empty()) {
    current_liquidity = std::strtod(pool_state.liquidity.c_str(), nullptr);
  }
  if (!std::isfinite(current_liquidity) || current_liquidity < 0.0) {
    current_liquidity = 0.0;
  }

  const double fee_mult = is_bid ? (1.0 - std::abs(fee_rate))
                                 : (1.0 + std::abs(fee_rate));

  // Find the tick nearest to current tick in the sorted ticks array.
  // Ticks are sorted ascending by tick index.
  const int64_t current_tick = pool_state.tick;
  const auto& ticks = pool_state.ticks;

  if (is_bid) {
    // Walk downward from current tick.
    // Find the first initialized tick <= current_tick.
    double liq = current_liquidity;

    // Find starting index: largest tick <= current_tick.
    auto it = std::upper_bound(ticks.begin(), ticks.end(), current_tick,
                               [](int64_t val, const VenuePoolTickLevel& t) {
                                 return val < t.tick;
                               });
    // it points to first tick > current_tick. Go back.

    while (it != ticks.begin() && out.size() < max_levels) {
      --it;
      const int64_t tick_lower = it->tick;

      // The tick range is [tick_lower, tick_upper).
      // For the first iteration, tick_upper = current_tick.
      // For subsequent ones, tick_upper = previous tick_lower.
      const int64_t tick_upper = (out.empty()) ? current_tick : (it + 1)->tick;

      const double p_lower = PriceAtTick(tick_lower);
      const double p_upper = PriceAtTick(tick_upper);

      if (liq > 0.0 && p_lower > 0.0 && p_upper > p_lower) {
        const double qty = BaseAmountInRange(liq, p_lower, p_upper);
        if (qty > 0.0) {
          const double level_price = p_lower * fee_mult;
          if (level_price > 0.0 && std::isfinite(level_price)) {
            out.push_back(VenueBookLevel{
                .price = DecimalFromDouble(level_price, price_scale),
                .qty = DecimalFromDouble(qty, qty_scale),
            });
          }
        }
      }

      // Cross the tick: subtract liquidityNet (going down).
      liq -= static_cast<double>(it->liquidity_net.units) *
             std::pow(10.0, -it->liquidity_net.scale);
      if (liq < 0.0) liq = 0.0;
    }

    // Sort bids descending by price.
    std::sort(out.begin(), out.end(),
              [](const VenueBookLevel& a, const VenueBookLevel& b) {
                return a.price.units > b.price.units;
              });
  } else {
    // Walk upward from current tick.
    double liq = current_liquidity;

    auto it = std::lower_bound(ticks.begin(), ticks.end(), current_tick,
                               [](const VenuePoolTickLevel& t, int64_t val) {
                                 return t.tick < val;
                               });

    while (it != ticks.end() && out.size() < max_levels) {
      const int64_t tick_lower = it->tick;

      // Cross the tick: add liquidityNet (going up).
      liq += static_cast<double>(it->liquidity_net.units) *
             std::pow(10.0, -it->liquidity_net.scale);
      if (liq < 0.0) liq = 0.0;

      ++it;
      if (it == ticks.end()) break;

      const int64_t tick_upper = it->tick;
      const double p_lower = PriceAtTick(tick_lower);
      const double p_upper = PriceAtTick(tick_upper);

      if (liq > 0.0 && p_upper > p_lower) {
        const double qty = BaseAmountInRange(liq, p_lower, p_upper);
        if (qty > 0.0) {
          const double level_price = p_upper * fee_mult;
          if (level_price > 0.0 && std::isfinite(level_price)) {
            out.push_back(VenueBookLevel{
                .price = DecimalFromDouble(level_price, price_scale),
                .qty = DecimalFromDouble(qty, qty_scale),
            });
          }
        }
      }
    }

    // Sort asks ascending by price.
    std::sort(out.begin(), out.end(),
              [](const VenueBookLevel& a, const VenueBookLevel& b) {
                return a.price.units < b.price.units;
              });
  }

  return out;
}

std::vector<VenueBookLevel> BuildUniformSide(
    double mid_price,
    double fee_rate,
    double base_liquidity,
    int32_t price_scale,
    int32_t qty_scale,
    std::size_t levels,
    bool is_bid) {
  std::vector<VenueBookLevel> out;
  levels = std::max<std::size_t>(1, levels);
  out.reserve(levels);

  const double min_price_step = std::pow(10.0, -std::max<int32_t>(0, price_scale));
  const double min_qty_step = std::pow(10.0, -std::max<int32_t>(0, qty_scale));

  const double safe_mid = std::max(mid_price, min_price_step);
  const double fee = std::max(0.0, std::min(1.0, std::abs(fee_rate)));
  const double near_price = is_bid ? safe_mid * (1.0 - fee)
                                   : safe_mid * (1.0 + fee);
  const double price_step = std::max(min_price_step, safe_mid * 0.0005);

  double base_qty = base_liquidity;
  if (!std::isfinite(base_qty) || base_qty <= 0.0) {
    base_qty = min_qty_step * static_cast<double>(levels) * 10.0;
  }
  base_qty = std::max(min_qty_step, base_qty / static_cast<double>(levels));

  for (std::size_t i = 0; i < levels; ++i) {
    const double price = is_bid
        ? (near_price - static_cast<double>(i) * price_step)
        : (near_price + static_cast<double>(i) * price_step);
    if (price <= 0.0 || !std::isfinite(price)) continue;

    const double qty_decay = 1.0 - (0.5 * static_cast<double>(i) /
                                    static_cast<double>(std::max<std::size_t>(1, levels)));
    const double qty = std::max(min_qty_step, base_qty * qty_decay);

    out.push_back(VenueBookLevel{
        .price = DecimalFromDouble(price, price_scale),
        .qty = DecimalFromDouble(qty, qty_scale),
    });
  }

  if (is_bid) {
    std::sort(out.begin(), out.end(),
              [](const VenueBookLevel& a, const VenueBookLevel& b) {
                return a.price.units > b.price.units;
              });
  } else {
    std::sort(out.begin(), out.end(),
              [](const VenueBookLevel& a, const VenueBookLevel& b) {
                return a.price.units < b.price.units;
              });
  }

  return out;
}

VirtualLob BuildVirtualLOBFromAMM(const AmmPoolExtractResult& pool,
                                  const VirtualLobConfig& config) {
  VirtualLob lob;

  if (!pool.valid || pool.mid_price <= 0.0) return lob;

  lob.mid_price = pool.mid_price;

  const bool has_ticks = !pool.pool_state.ticks.empty() &&
                         !pool.pool_state.liquidity.empty();

  if (has_ticks) {
    // Strategy 1: tick-based synthesis from concentrated liquidity.
    lob.bids = BuildTickBasedSide(
        pool.pool_state, pool.mid_price, config.fee_rate,
        config.price_scale, config.qty_scale,
        config.levels_per_side, true);

    lob.asks = BuildTickBasedSide(
        pool.pool_state, pool.mid_price, config.fee_rate,
        config.price_scale, config.qty_scale,
        config.levels_per_side, false);
  }

  // Fallback to uniform if tick-based produced nothing.
  if (lob.bids.empty() || lob.asks.empty()) {
    const double base_liq = static_cast<double>(pool.reserve_base);
    lob.bids = BuildUniformSide(
        pool.mid_price, config.fee_rate, base_liq,
        config.price_scale, config.qty_scale,
        config.levels_per_side, true);

    lob.asks = BuildUniformSide(
        pool.mid_price, config.fee_rate, base_liq,
        config.price_scale, config.qty_scale,
        config.levels_per_side, false);
  }

  return lob;
}

}  // namespace cex::venues::domain
