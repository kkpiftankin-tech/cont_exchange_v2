#include "domain/amm_pool_extractor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace cex::venues::domain {

namespace {

// Try to find a field by primary key or fallback key(s).
std::string FindField(
    const std::unordered_map<std::string, std::string>& fields,
    const std::string& key1,
    const std::string& key2 = {},
    const std::string& key3 = {}) {
  auto it = fields.find(key1);
  if (it != fields.end() && !it->second.empty()) return it->second;
  if (!key2.empty()) {
    it = fields.find(key2);
    if (it != fields.end() && !it->second.empty()) return it->second;
  }
  if (!key3.empty()) {
    it = fields.find(key3);
    if (it != fields.end() && !it->second.empty()) return it->second;
  }
  return {};
}

int64_t ParseInt64(const std::string& text) {
  if (text.empty()) return 0;
  char* end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return 0;
  return static_cast<int64_t>(value);
}

uint64_t ParseUint64(const std::string& text) {
  if (text.empty()) return 0;

  std::string s = text;
  // Support hex (0x...) format for block numbers.
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    char* end = nullptr;
    const unsigned long long hex = std::strtoull(s.c_str() + 2, &end, 16);
    if (end == nullptr || *end != '\0') return 0;
    return static_cast<uint64_t>(hex);
  }

  char* end = nullptr;
  const unsigned long long dec = std::strtoull(s.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return 0;
  return static_cast<uint64_t>(dec);
}

bool ParseDecimalToScale(const std::string& text, int32_t target_scale,
                         int64_t* out_units) {
  if (text.empty() || out_units == nullptr) return false;

  // Parse as double and scale.
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(value)) return false;

  const double scaled = value * std::pow(10.0, target_scale);
  if (std::abs(scaled) > static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return false;
  }

  *out_units = static_cast<int64_t>(std::llround(scaled));
  return true;
}

bool ParseBool(const std::string& text, bool default_val) {
  if (text.empty()) return default_val;
  if (text == "true" || text == "1") return true;
  if (text == "false" || text == "0") return false;
  return default_val;
}

}  // namespace

AmmPoolExtractor::AmmPoolExtractor(const AmmPoolExtractorConfig& config)
    : config_(config) {}

// --- Static price functions ---

double AmmPoolExtractor::MidPriceFromSqrtPriceX96(
    const std::string& sqrt_price_x96) {
  if (sqrt_price_x96.empty()) return 0.0;

  const long double sqrt_q96 = std::strtold(sqrt_price_x96.c_str(), nullptr);
  if (sqrt_q96 <= 0.0L) return 0.0;

  constexpr long double kTwoPow96 =
      79'228'162'514'264'337'593'543'950'336.0L;
  const long double sqrt_price = sqrt_q96 / kTwoPow96;
  const long double price = sqrt_price * sqrt_price;

  const double result = static_cast<double>(price);
  if (!std::isfinite(result) || result <= 0.0) return 0.0;
  return result;
}

double AmmPoolExtractor::MidPriceFromTick(int64_t tick) {
  if (tick == 0) return 0.0;
  const double price = std::pow(1.0001, static_cast<double>(tick));
  if (!std::isfinite(price) || price <= 0.0) return 0.0;
  return price;
}

double AmmPoolExtractor::MidPriceFromReserves(
    const cex::common::Decimal& reserve_base,
    const cex::common::Decimal& reserve_quote) {
  const double base = static_cast<double>(reserve_base);
  const double quote = static_cast<double>(reserve_quote);
  if (base <= 0.0 || quote <= 0.0) return 0.0;
  const double price = quote / base;
  if (!std::isfinite(price) || price <= 0.0) return 0.0;
  return price;
}

double AmmPoolExtractor::ResolveMidPrice(
    const VenuePoolState& pool_state,
    const cex::common::Decimal& reserve_base,
    const cex::common::Decimal& reserve_quote) {
  // Priority: sqrtPriceX96 > tick > reserves.
  double price = MidPriceFromSqrtPriceX96(pool_state.sqrt_price_x96);
  if (price > 0.0) return price;

  price = MidPriceFromTick(pool_state.tick);
  if (price > 0.0) return price;

  return MidPriceFromReserves(reserve_base, reserve_quote);
}

// --- Extraction ---

AmmPoolExtractResult AmmPoolExtractor::Extract(
    const std::unordered_map<std::string, std::string>& fields,
    const std::vector<VenuePoolTickLevel>& tick_levels) const {
  AmmPoolExtractResult result;
  auto& ps = result.pool_state;

  // Pool address.
  ps.pool_address = FindField(fields, "pool_address", "poolAddress", "pool");

  // sqrtPriceX96.
  ps.sqrt_price_x96 =
      FindField(fields, "sqrt_price_x96", "sqrtPriceX96");

  // Tick.
  const std::string tick_text = FindField(fields, "tick");
  if (!tick_text.empty()) {
    ps.tick = ParseInt64(tick_text);
  }

  // Liquidity.
  ps.liquidity = FindField(fields, "liquidity");

  // Block number.
  const std::string block_text =
      FindField(fields, "block_number", "blockNumber");
  if (!block_text.empty()) {
    ps.block_number = ParseUint64(block_text);
  }

  // Finalized.
  const std::string finalized_text =
      FindField(fields, "finalized", "isFinalized");
  ps.finalized = ParseBool(finalized_text, true);

  // Tick levels.
  ps.ticks = tick_levels;
  if (config_.max_tick_levels > 0 && ps.ticks.size() > config_.max_tick_levels) {
    ps.ticks.resize(config_.max_tick_levels);
  }
  // Sort by tick index ascending.
  std::sort(ps.ticks.begin(), ps.ticks.end(),
            [](const VenuePoolTickLevel& a, const VenuePoolTickLevel& b) {
              return a.tick < b.tick;
            });

  // Reserves.
  const std::string base_text =
      FindField(fields, "reserve_base", "reserve0", "baseReserve");
  const std::string quote_text =
      FindField(fields, "reserve_quote", "reserve1", "quoteReserve");

  int64_t base_units = 0;
  int64_t quote_units = 0;
  if (ParseDecimalToScale(base_text, config_.qty_scale, &base_units)) {
    result.reserve_base = cex::common::Decimal{base_units, config_.qty_scale};
  }
  if (ParseDecimalToScale(quote_text, config_.qty_scale, &quote_units)) {
    result.reserve_quote =
        cex::common::Decimal{quote_units, config_.qty_scale};
  }

  // Validate: at least sqrtPriceX96 or tick or reserves must be present.
  const bool has_sqrt = !ps.sqrt_price_x96.empty();
  const bool has_tick = ps.tick != 0;
  const bool has_reserves =
      result.reserve_base.units > 0 && result.reserve_quote.units > 0;

  if (!has_sqrt && !has_tick && !has_reserves) {
    result.error = "no price source: sqrtPriceX96, tick, and reserves all empty";
    return result;
  }

  // Compute mid price.
  result.mid_price = ResolveMidPrice(ps, result.reserve_base, result.reserve_quote);
  if (result.mid_price <= 0.0) {
    result.error = "failed to compute valid mid price";
    return result;
  }

  // Sequence from block number.
  result.sequence = ps.block_number;

  result.valid = true;
  return result;
}

// --- Caching ---

bool AmmPoolExtractor::ApplyToCache(const std::string& pool_key,
                                    const AmmPoolExtractResult& result) {
  if (!result.valid) return false;

  auto it = cache_.find(pool_key);
  if (it != cache_.end()) {
    // Reject stale updates: block number must be >= cached.
    if (result.pool_state.block_number > 0 &&
        it->second.pool_state.block_number > 0 &&
        result.pool_state.block_number < it->second.pool_state.block_number) {
      return false;
    }
    // Reject exact duplicate block.
    if (result.pool_state.block_number > 0 &&
        result.pool_state.block_number == it->second.pool_state.block_number &&
        result.pool_state.sqrt_price_x96 == it->second.pool_state.sqrt_price_x96) {
      return false;
    }
  }

  cache_[pool_key] = result;
  // Update sequence to be strictly increasing.
  auto& cached = cache_[pool_key];
  if (it != cache_.end() && cached.sequence <= it->second.sequence) {
    cached.sequence = it->second.sequence + 1;
  }

  return true;
}

std::optional<AmmPoolExtractResult> AmmPoolExtractor::GetCached(
    const std::string& pool_key) const {
  auto it = cache_.find(pool_key);
  if (it == cache_.end()) return std::nullopt;
  return it->second;
}

void AmmPoolExtractor::ClearCache() {
  cache_.clear();
}

}  // namespace cex::venues::domain
