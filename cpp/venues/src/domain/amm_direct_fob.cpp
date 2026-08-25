#include "domain/amm_direct_fob.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace cex::venues::domain {

namespace {

struct AmmExecutionSegment {
  double price{0.0};
  double qty{0.0};
};

double ParseDoubleText(const std::string& text) {
  if (text.empty()) return 0.0;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(value)) return 0.0;
  return value;
}

cex::common::Decimal DecimalFromDouble(const double value, const int32_t scale) {
  if (!std::isfinite(value)) return cex::common::Decimal{0, scale};
  const double factor = std::pow(10.0, static_cast<double>(scale));
  const double scaled = std::round(value * factor);
  if (scaled > static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return cex::common::Decimal{std::numeric_limits<int64_t>::max(), scale};
  }
  if (scaled < static_cast<double>(std::numeric_limits<int64_t>::min())) {
    return cex::common::Decimal{std::numeric_limits<int64_t>::min(), scale};
  }
  return cex::common::Decimal{
      .units = static_cast<int64_t>(scaled),
      .scale = scale,
  };
}

double PriceAtTick(const int64_t tick) {
  const double price = std::pow(1.0001, static_cast<double>(tick));
  if (!std::isfinite(price) || price <= 0.0) return 0.0;
  return price;
}

double BaseAmountInRange(const double liquidity,
                         const double price_lower,
                         const double price_upper) {
  if (liquidity <= 0.0 || price_lower <= 0.0 || price_upper <= 0.0) return 0.0;
  if (price_lower >= price_upper) return 0.0;

  const double sqrt_lower = std::sqrt(price_lower);
  const double sqrt_upper = std::sqrt(price_upper);
  if (sqrt_lower <= 0.0 || sqrt_upper <= 0.0) return 0.0;

  const double qty = liquidity * (1.0 / sqrt_lower - 1.0 / sqrt_upper);
  if (!std::isfinite(qty) || qty <= 0.0) return 0.0;
  return qty;
}

double LiquidityNetAsDouble(const VenuePoolTickLevel& level) {
  return static_cast<double>(level.liquidity_net);
}

std::vector<AmmExecutionSegment> BuildSideSegmentsFromTicks(
    const VenuePoolState& pool_state,
    const ExecutionSide side,
    const DirectAmmFobConfig& config) {
  std::vector<AmmExecutionSegment> out;
  const std::size_t max_segments =
      std::max<std::size_t>(1, config.max_segments_per_side);
  if (pool_state.ticks.empty()) return out;

  double liquidity = ParseDoubleText(pool_state.liquidity);
  if (!std::isfinite(liquidity) || liquidity <= 0.0) return out;

  const auto& ticks = pool_state.ticks;
  const int64_t current_tick = pool_state.tick;

  out.reserve(max_segments);

  if (side == ExecutionSide::kSell) {
    // Bid side: walk downward from current tick.
    auto it = std::upper_bound(
        ticks.begin(),
        ticks.end(),
        current_tick,
        [](const int64_t value, const VenuePoolTickLevel& level) {
          return value < level.tick;
        });

    while (it != ticks.begin() && out.size() < max_segments) {
      --it;
      const int64_t tick_lower = it->tick;
      const int64_t tick_upper = out.empty() ? current_tick : (it + 1)->tick;
      if (tick_upper <= tick_lower) continue;

      const double p_lower = PriceAtTick(tick_lower);
      const double p_upper = PriceAtTick(tick_upper);
      const double qty = BaseAmountInRange(liquidity, p_lower, p_upper);
      const double price = p_lower;
      if (qty > 0.0 && price > 0.0 && std::isfinite(price)) {
        out.push_back(AmmExecutionSegment{
            .price = price,
            .qty = qty,
        });
      }

      liquidity -= LiquidityNetAsDouble(*it);
      if (liquidity < 0.0) liquidity = 0.0;
    }
  } else {
    // Ask side: walk upward from current tick.
    auto it = std::lower_bound(
        ticks.begin(),
        ticks.end(),
        current_tick,
        [](const VenuePoolTickLevel& level, const int64_t value) {
          return level.tick < value;
        });

    while (it != ticks.end() && out.size() < max_segments) {
      const int64_t tick_lower = it->tick;
      liquidity += LiquidityNetAsDouble(*it);
      if (liquidity < 0.0) liquidity = 0.0;

      ++it;
      if (it == ticks.end()) break;

      const int64_t tick_upper = it->tick;
      if (tick_upper <= tick_lower) continue;

      const double p_lower = PriceAtTick(tick_lower);
      const double p_upper = PriceAtTick(tick_upper);
      const double qty = BaseAmountInRange(liquidity, p_lower, p_upper);
      const double price = p_upper;
      if (qty > 0.0 && price > 0.0 && std::isfinite(price)) {
        out.push_back(AmmExecutionSegment{
            .price = price,
            .qty = qty,
        });
      }
    }
  }

  return out;
}

std::vector<AmmExecutionSegment> ApplyExecutionCorrections(
    const std::vector<AmmExecutionSegment>& base_segments,
    const ExecutionSide side,
    const DirectAmmFobConfig& config) {
  std::vector<AmmExecutionSegment> out;
  if (base_segments.empty()) return out;

  const double pool_fee_rate = std::clamp(std::abs(config.pool_fee_rate), 0.0, 0.999999);
  const double overhead_rate = std::clamp(
      std::abs(config.execution_overhead_bps) / 10000.0, 0.0, 0.999999);
  const double slippage_multiplier =
      (std::isfinite(config.slippage_multiplier) && config.slippage_multiplier > 0.0)
      ? config.slippage_multiplier
      : 1.0;

  const double anchor_price = base_segments.front().price;
  if (!std::isfinite(anchor_price) || anchor_price <= 0.0) return out;

  out.reserve(base_segments.size());

  const double buy_multiplier = (1.0 + pool_fee_rate) * (1.0 + overhead_rate);
  const double sell_multiplier =
      std::max(0.0, (1.0 - pool_fee_rate) * (1.0 - overhead_rate));

  for (const auto& segment : base_segments) {
    if (!std::isfinite(segment.price) || segment.price <= 0.0 ||
        !std::isfinite(segment.qty) || segment.qty <= 0.0) {
      continue;
    }

    const double impacted_price =
        anchor_price + (segment.price - anchor_price) * slippage_multiplier;
    double corrected_price = 0.0;
    if (side == ExecutionSide::kBuy) {
      corrected_price = impacted_price * buy_multiplier;
    } else {
      corrected_price = impacted_price * sell_multiplier;
    }

    if (!std::isfinite(corrected_price) || corrected_price <= 0.0) continue;
    if (!out.empty()) {
      if (side == ExecutionSide::kBuy &&
          corrected_price < out.back().price) {
        corrected_price = out.back().price;
      }
      if (side == ExecutionSide::kSell &&
          corrected_price > out.back().price) {
        corrected_price = out.back().price;
      }
    }

    out.push_back(AmmExecutionSegment{
        .price = corrected_price,
        .qty = segment.qty,
    });
  }
  return out;
}

}  // namespace

DepthSideCurves BuildDirectAmmFobCurves(
    const VenuePoolState& pool_state,
    const ExecutionSide side,
    const cex::common::Decimal& tau_sec,
    const DirectAmmFobConfig& config) {
  DepthSideCurves out;
  out.side = side;
  out.tau_sec = (tau_sec.units > 0) ? tau_sec : cex::common::Decimal{1, 0};

  const auto base_segments = BuildSideSegmentsFromTicks(pool_state, side, config);
  const auto segments = ApplyExecutionCorrections(base_segments, side, config);
  if (segments.empty()) return out;

  const int32_t qty_scale = std::max<int32_t>(0, config.qty_scale);
  const int32_t price_scale = std::max<int32_t>(0, config.price_scale);
  const int32_t cost_scale = std::clamp<int32_t>(qty_scale + price_scale, 0, 14);
  const double gas_cost_quote = std::max(0.0, config.gas_cost_quote);

  std::vector<SOfQPoint> s_of_q;
  s_of_q.reserve(segments.size() + 1);

  double cumulative_qty = 0.0;
  double cumulative_cost = 0.0;
  s_of_q.push_back(SOfQPoint{
      .qty = cex::common::Decimal{0, qty_scale},
      .cumulative_cost = cex::common::Decimal{0, cost_scale},
  });

  for (const auto& seg : segments) {
    if (!std::isfinite(seg.price) || !std::isfinite(seg.qty) ||
        seg.price <= 0.0 || seg.qty <= 0.0) {
      continue;
    }
    cumulative_qty += seg.qty;
    const double segment_cost = seg.price * seg.qty;
    if (!std::isfinite(segment_cost) || segment_cost <= 0.0) continue;
    cumulative_cost += segment_cost;
    if (!std::isfinite(cumulative_cost) || cumulative_cost <= 0.0) continue;

    const cex::common::Decimal q = DecimalFromDouble(cumulative_qty, qty_scale);
    double adjusted_cost = cumulative_cost;
    if (gas_cost_quote > 0.0) {
      if (side == ExecutionSide::kBuy) {
        adjusted_cost += gas_cost_quote;
      } else {
        adjusted_cost = std::max(0.0, adjusted_cost - gas_cost_quote);
      }
    }
    if (!std::isfinite(adjusted_cost)) continue;

    cex::common::Decimal s = DecimalFromDouble(adjusted_cost, cost_scale);
    if (!s_of_q.empty() &&
        cex::common::Decimal::cmp(s, s_of_q.back().cumulative_cost) < 0) {
      s = s_of_q.back().cumulative_cost;
    }
    s_of_q.push_back(SOfQPoint{
        .qty = q,
        .cumulative_cost = s,
    });
  }

  if (s_of_q.size() < 2) return out;

  // Provide desired output price scale hint for RebuildFromCostLayer.
  out.p_of_q.push_back(POfQPoint{
      .qty = cex::common::Decimal{0, qty_scale},
      .price = DecimalFromDouble(segments.front().price, price_scale),
  });
  return RebuildFromCostLayer(out, s_of_q);
}

}  // namespace cex::venues::domain
