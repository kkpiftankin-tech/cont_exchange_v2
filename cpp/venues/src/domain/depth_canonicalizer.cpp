#include "domain/depth_canonicalizer.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace cex::venues::domain {

namespace {

struct NormalizedLevel {
  int64_t price_units{0};
  int64_t qty_units{0};
};

constexpr int32_t kMaxScale = 18;

int64_t pow10_i64(const int32_t p) {
  if (p < 0 || p > kMaxScale) return 0;
  int64_t out = 1;
  for (int32_t i = 0; i < p; ++i) {
    if (out > std::numeric_limits<int64_t>::max() / 10) return 0;
    out *= 10;
  }
  return out;
}

bool align_to_scale(const cex::common::Decimal& value,
                    const int32_t target_scale,
                    int64_t* out_units) {
  if (out_units == nullptr || target_scale < 0) return false;

  if (target_scale == value.scale) {
    *out_units = value.units;
    return true;
  }

  if (target_scale < value.scale) {
    const int32_t delta = value.scale - target_scale;
    const int64_t div = pow10_i64(delta);
    if (div == 0) return false;
    *out_units = value.units / div;
    return true;
  }

  const int32_t delta = target_scale - value.scale;
  const int64_t mul = pow10_i64(delta);
  if (mul == 0) return false;

  const __int128 scaled = static_cast<__int128>(value.units) * static_cast<__int128>(mul);
  if (scaled > std::numeric_limits<int64_t>::max() ||
      scaled < std::numeric_limits<int64_t>::min()) {
    return false;
  }

  *out_units = static_cast<int64_t>(scaled);
  return true;
}

int64_t floor_to_step(const int64_t value_units, const int64_t step_units) {
  if (step_units <= 0) return value_units;
  if (value_units >= 0) return (value_units / step_units) * step_units;
  return -(((-value_units + step_units - 1) / step_units) * step_units);
}

int64_t ceil_to_step(const int64_t value_units, const int64_t step_units) {
  if (step_units <= 0) return value_units;
  if (value_units >= 0) return ((value_units + step_units - 1) / step_units) * step_units;
  return -(((-value_units) / step_units) * step_units);
}

int32_t detect_price_scale(const std::vector<BookLevel>& levels,
                           const DepthCanonicalizationConfig& config) {
  int32_t scale = 0;
  if (config.tick_size.units > 0) scale = std::max(scale, config.tick_size.scale);
  for (const auto& level : levels) {
    scale = std::max(scale, level.price.scale);
  }
  return scale;
}

int32_t detect_qty_scale(const std::vector<BookLevel>& levels,
                         const DepthCanonicalizationConfig& config) {
  int32_t scale = 0;
  if (config.lot_size.units > 0) scale = std::max(scale, config.lot_size.scale);
  if (config.min_qty.units > 0) scale = std::max(scale, config.min_qty.scale);
  for (const auto& level : levels) {
    scale = std::max(scale, level.qty.scale);
  }
  return scale;
}

}  // namespace

std::vector<BookLevel> CanonicalizeBookSide(const std::vector<BookLevel>& levels,
                                            const BookSide side,
                                            const DepthCanonicalizationConfig& config) {
  const int32_t price_scale = detect_price_scale(levels, config);
  const int32_t qty_scale = detect_qty_scale(levels, config);

  int64_t tick_units = 0;
  if (config.tick_size.units > 0 && !align_to_scale(config.tick_size, price_scale, &tick_units)) {
    tick_units = 0;
  }
  if (tick_units <= 0) tick_units = 0;

  int64_t lot_units = 0;
  if (config.lot_size.units > 0 && !align_to_scale(config.lot_size, qty_scale, &lot_units)) {
    lot_units = 0;
  }
  if (lot_units <= 0) lot_units = 0;

  int64_t min_qty_units = 0;
  if (config.min_qty.units > 0 && !align_to_scale(config.min_qty, qty_scale, &min_qty_units)) {
    min_qty_units = 0;
  }
  if (min_qty_units <= 0) min_qty_units = 0;

  std::vector<NormalizedLevel> normalized;
  normalized.reserve(levels.size());

  for (const auto& level : levels) {
    if (level.price.units <= 0 || level.qty.units <= 0) continue;

    int64_t price_units = 0;
    int64_t qty_units = 0;
    if (!align_to_scale(level.price, price_scale, &price_units)) continue;
    if (!align_to_scale(level.qty, qty_scale, &qty_units)) continue;

    if (tick_units > 0) {
      if (side == BookSide::kBid) {
        price_units = floor_to_step(price_units, tick_units);
      } else {
        price_units = ceil_to_step(price_units, tick_units);
      }
    }

    if (lot_units > 0) qty_units = floor_to_step(qty_units, lot_units);

    if (price_units <= 0 || qty_units <= 0) continue;
    if (min_qty_units > 0 && qty_units < min_qty_units) continue;

    normalized.push_back(NormalizedLevel{
        .price_units = price_units,
        .qty_units = qty_units,
    });
  }

  if (normalized.empty()) return {};

  std::sort(normalized.begin(), normalized.end(),
            [side](const NormalizedLevel& lhs, const NormalizedLevel& rhs) {
              if (lhs.price_units == rhs.price_units) return lhs.qty_units > rhs.qty_units;
              if (side == BookSide::kBid) return lhs.price_units > rhs.price_units;
              return lhs.price_units < rhs.price_units;
            });

  std::vector<NormalizedLevel> aggregated;
  aggregated.reserve(normalized.size());

  for (const auto& level : normalized) {
    if (!aggregated.empty() && aggregated.back().price_units == level.price_units) {
      aggregated.back().qty_units += level.qty_units;
      continue;
    }
    aggregated.push_back(level);
  }

  if (config.max_levels_per_side > 0 && aggregated.size() > config.max_levels_per_side) {
    aggregated.resize(config.max_levels_per_side);
  }

  std::vector<BookLevel> out;
  out.reserve(aggregated.size());
  for (const auto& level : aggregated) {
    out.push_back(BookLevel{
        .price = cex::common::Decimal{.units = level.price_units, .scale = price_scale},
        .qty = cex::common::Decimal{.units = level.qty_units, .scale = qty_scale},
    });
  }
  return out;
}

CanonicalOrderBook CanonicalizeOrderBook(const std::vector<BookLevel>& bids,
                                         const std::vector<BookLevel>& asks,
                                         const DepthCanonicalizationConfig& config) {
  return CanonicalOrderBook{
      .bids = CanonicalizeBookSide(bids, BookSide::kBid, config),
      .asks = CanonicalizeBookSide(asks, BookSide::kAsk, config),
  };
}

}  // namespace cex::venues::domain
