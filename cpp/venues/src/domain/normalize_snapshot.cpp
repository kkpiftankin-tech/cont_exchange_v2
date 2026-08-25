#include "domain/normalize_snapshot.hpp"

#include <cstdint>
#include <limits>
#include <sstream>

#include "cex/common/decimal.hpp"
#include "cex/common/uuid.hpp"
#include "cex/common/time.hpp"

namespace cex::venues::domain {

namespace {

int64_t pow10(int32_t p) {
  if (p < 0 || p > 18) return 0;
  int64_t out = 1;
  for (int32_t i = 0; i < p; ++i) {
    if (out > std::numeric_limits<int64_t>::max() / 10) return 0;
    out *= 10;
  }
  return out;
}

// Convert VenueBookLevel vector to canonicalizer BookLevel vector.
std::vector<BookLevel> ToBookLevels(const std::vector<VenueBookLevel>& levels) {
  std::vector<BookLevel> out;
  out.reserve(levels.size());
  for (const auto& l : levels) {
    out.push_back(BookLevel{.price = l.price, .qty = l.qty});
  }
  return out;
}

std::string EncodePoolTicks(const std::vector<VenuePoolTickLevel>& ticks) {
  // Compact text format: "<tick>,<liq_units>,<liq_scale>;..."
  // Designed for internal metadata transport only.
  std::ostringstream ss;
  bool first = true;
  for (const auto& level : ticks) {
    if (!first) ss << ';';
    first = false;
    ss << level.tick << ','
       << level.liquidity_net.units << ','
       << level.liquidity_net.scale;
  }
  return ss.str();
}

}  // namespace

cex::common::Decimal QuantizeDecimal(const cex::common::Decimal& value,
                                     int32_t target_scale) {
  if (target_scale < 0) target_scale = 0;
  if (value.scale == target_scale) return value;

  if (target_scale < value.scale) {
    // Reduce precision: divide units.
    const int32_t delta = value.scale - target_scale;
    const int64_t div = pow10(delta);
    if (div == 0) return value;
    return cex::common::Decimal{value.units / div, target_scale};
  }

  // Increase precision: multiply units.
  const int32_t delta = target_scale - value.scale;
  const int64_t mul = pow10(delta);
  if (mul == 0) return value;

  const __int128 scaled =
      static_cast<__int128>(value.units) * static_cast<__int128>(mul);
  if (scaled > std::numeric_limits<int64_t>::max() ||
      scaled < std::numeric_limits<int64_t>::min()) {
    return value;  // Overflow — keep original.
  }
  return cex::common::Decimal{static_cast<int64_t>(scaled), target_scale};
}

fob::venue::v1::VenueSnapshot NormalizeSnapshot(
    const VenueRawSnapshot& raw,
    const NormalizationConfig& config) {
  using cex::common::Decimal;

  fob::venue::v1::VenueSnapshot snap;

  // --- EventMeta ---
  auto* meta = snap.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("venues");
  meta->set_correlation_id(cex::common::uuid_v4());
  meta->set_partition_key(raw.venue_id + "|" + raw.instrument.symbol());
  (*meta->mutable_tags())["venue_type"] = ToString(raw.venue_type);

  // --- Identity ---
  snap.set_venue_id(raw.venue_id);
  *snap.mutable_instrument() = raw.instrument;
  *snap.mutable_timestamp() = raw.timestamp;

  // --- BBO: compute mid and spread from raw best_bid/best_ask ---
  const Decimal best_bid = raw.best_bid;
  const Decimal best_ask = raw.best_ask;

  // mid = (best_bid + best_ask) / 2
  Decimal mid = Decimal::zero();
  Decimal spread = Decimal::zero();
  if (best_bid.units > 0 && best_ask.units > 0) {
    const Decimal sum = Decimal::add(best_bid, best_ask);
    // Divide by 2: double the scale, keep units, then adjust.
    // Simple approach: use (units/2, scale) — truncation is acceptable.
    mid = Decimal{sum.units / 2, sum.scale};
    spread = Decimal::sub(best_ask, best_bid);
  }

  *snap.mutable_best_bid() = best_bid.to_proto();
  *snap.mutable_best_ask() = best_ask.to_proto();
  *snap.mutable_mid_price() = mid.to_proto();
  *snap.mutable_spread() = spread.to_proto();

  // --- Depth: canonicalize using tick/lot from trading_rules ---
  DepthCanonicalizationConfig depth_cfg = config.depth_config;
  // If tick/lot not set in config, use the instrument's trading rules.
  if (depth_cfg.tick_size.units == 0 && raw.trading_rules.tick_size.units > 0) {
    depth_cfg.tick_size = raw.trading_rules.tick_size;
  }
  if (depth_cfg.lot_size.units == 0 && raw.trading_rules.lot_size.units > 0) {
    depth_cfg.lot_size = raw.trading_rules.lot_size;
  }
  if (depth_cfg.min_qty.units == 0 && raw.trading_rules.min_qty.units > 0) {
    depth_cfg.min_qty = raw.trading_rules.min_qty;
  }

  const auto book = CanonicalizeOrderBook(
      ToBookLevels(raw.bids), ToBookLevels(raw.asks), depth_cfg);

  for (const auto& level : book.bids) {
    *snap.add_bid_prices() = level.price.to_proto();
    *snap.add_bid_quantities() = level.qty.to_proto();
  }
  for (const auto& level : book.asks) {
    *snap.add_ask_prices() = level.price.to_proto();
    *snap.add_ask_quantities() = level.qty.to_proto();
  }

  // --- F11-NORM-2: fee/tick/lot normalization ---
  const int32_t fee_scale = config.fee_scale;
  *snap.mutable_maker_fee() =
      QuantizeDecimal(raw.fees.maker, fee_scale).to_proto();
  *snap.mutable_taker_fee() =
      QuantizeDecimal(raw.fees.taker, fee_scale).to_proto();

  // tick_size and lot_size: keep their own scale (already from trading_rules).
  *snap.mutable_tick_size() = raw.trading_rules.tick_size.to_proto();
  *snap.mutable_lot_size() = raw.trading_rules.lot_size.to_proto();

  // --- Status ---
  snap.set_status(ToString(raw.status));

  // --- Volume ---
  *snap.mutable_volume_24h() = raw.volume_24h.to_proto();

  // Preserve AMM pool-state details in metadata tags for direct AMM->FOB path.
  if (raw.pool_state.has_value()) {
    const auto& pool = *raw.pool_state;
    auto* tags = meta->mutable_tags();
    (*tags)["amm.pool_state.present"] = "true";
    (*tags)["amm.pool_address"] = pool.pool_address;
    (*tags)["amm.sqrt_price_x96"] = pool.sqrt_price_x96;
    (*tags)["amm.tick"] = std::to_string(pool.tick);
    (*tags)["amm.liquidity"] = pool.liquidity;
    (*tags)["amm.block_number"] = std::to_string(pool.block_number);
    (*tags)["amm.finalized"] = pool.finalized ? "true" : "false";
    (*tags)["amm.ticks"] = EncodePoolTicks(pool.ticks);
  } else {
    (*meta->mutable_tags())["amm.pool_state.present"] = "false";
  }

  return snap;
}

}  // namespace cex::venues::domain
