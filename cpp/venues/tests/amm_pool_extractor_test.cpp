#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/amm_pool_extractor.hpp"

namespace {

using cex::common::Decimal;
using cex::venues::domain::AmmPoolExtractorConfig;
using cex::venues::domain::AmmPoolExtractor;
using cex::venues::domain::AmmPoolExtractResult;
using cex::venues::domain::VenuePoolState;
using cex::venues::domain::VenuePoolTickLevel;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool CheckClose(double actual, double expected, double tolerance,
                const std::string& name) {
  if (std::abs(actual - expected) <= tolerance) return true;
  std::cerr << "[FAIL] " << name << " expected ~" << expected
            << " got " << actual << std::endl;
  return false;
}

AmmPoolExtractorConfig DefaultConfig() {
  return AmmPoolExtractorConfig{
      .price_scale = 8,
      .qty_scale = 8,
      .max_tick_levels = 256,
  };
}

std::unordered_map<std::string, std::string> MakeUniV3Fields() {
  return {
      {"pool_address", "0xabc123"},
      {"sqrtPriceX96", "1461446703485210103287273052203988822378723970341"},
      {"tick", "202919"},
      {"liquidity", "123456789000"},
      {"block_number", "19000000"},
      {"finalized", "true"},
  };
}

// sqrtPriceX96 = 1461446703485210103287273052203988822378723970341
// This corresponds to a large price. Let's use a more realistic one.
// For BTC/USDT ~70000: sqrtPriceX96 ≈ 20952466866399458816 * 2^32 ≈ ...
// Let's just use a known sqrtPriceX96 for ETH/USDC ~3000:
// sqrtPriceX96 = 4339505179874779 * 2^48 → price ≈ 3000
// Actually, let's use: sqrtPriceX96 = "4339505179874779000000000000"
// price = (4339505179874779000000000000 / 2^96)^2
// More practical: use a realistic value and verify MidPriceFromSqrtPriceX96.
//
// For unit testing, the simplest approach is:
// sqrtPriceX96 = 2^96 * sqrt(price)
// If price = 1.0, sqrtPriceX96 = 2^96 = 79228162514264337593543950336

std::unordered_map<std::string, std::string> MakeFieldsPrice1() {
  // price = 1.0 → sqrtPriceX96 = 2^96
  return {
      {"pool_address", "0xpool1"},
      {"sqrtPriceX96", "79228162514264337593543950336"},
      {"tick", "0"},
      {"liquidity", "1000000"},
      {"block_number", "100"},
      {"finalized", "true"},
  };
}

std::unordered_map<std::string, std::string> MakeFieldsPrice4() {
  // price = 4.0 → sqrtPriceX96 = 2^96 * 2 = 158456325028528675187087900672
  return {
      {"pool_address", "0xpool4"},
      {"sqrtPriceX96", "158456325028528675187087900672"},
      {"tick", "13863"},
      {"liquidity", "5000000"},
      {"block_number", "200"},
      {"finalized", "true"},
  };
}

// --- MidPriceFromSqrtPriceX96 tests ---

bool test_mid_price_from_sqrt_price_x96_unit() {
  // sqrtPriceX96 = 2^96 → price = 1.0
  double price = AmmPoolExtractor::MidPriceFromSqrtPriceX96(
      "79228162514264337593543950336");
  return CheckClose(price, 1.0, 0.001, "price=1.0");
}

bool test_mid_price_from_sqrt_price_x96_four() {
  // sqrtPriceX96 = 2^96 * 2 → price = 4.0
  double price = AmmPoolExtractor::MidPriceFromSqrtPriceX96(
      "158456325028528675187087900672");
  return CheckClose(price, 4.0, 0.01, "price=4.0");
}

bool test_mid_price_from_sqrt_price_x96_empty() {
  double price = AmmPoolExtractor::MidPriceFromSqrtPriceX96("");
  return Check(price == 0.0, "empty sqrtPriceX96 → 0");
}

bool test_mid_price_from_sqrt_price_x96_zero() {
  double price = AmmPoolExtractor::MidPriceFromSqrtPriceX96("0");
  return Check(price == 0.0, "zero sqrtPriceX96 → 0");
}

// --- MidPriceFromTick tests ---

bool test_mid_price_from_tick_positive() {
  // tick = 100 → price = 1.0001^100 ≈ 1.01005
  double price = AmmPoolExtractor::MidPriceFromTick(100);
  return CheckClose(price, std::pow(1.0001, 100.0), 0.0001, "tick=100");
}

bool test_mid_price_from_tick_negative() {
  // tick = -100 → price = 1.0001^(-100) ≈ 0.99005
  double price = AmmPoolExtractor::MidPriceFromTick(-100);
  return CheckClose(price, std::pow(1.0001, -100.0), 0.0001, "tick=-100");
}

bool test_mid_price_from_tick_zero() {
  double price = AmmPoolExtractor::MidPriceFromTick(0);
  return Check(price == 0.0, "tick=0 → 0");
}

// --- MidPriceFromReserves tests ---

bool test_mid_price_from_reserves() {
  // base=10, quote=30000 → price = 3000
  Decimal base{10'00000000, 8};      // 10.0 at scale 8
  Decimal quote{30000'00000000LL, 8}; // 30000.0 at scale 8
  double price = AmmPoolExtractor::MidPriceFromReserves(base, quote);
  return CheckClose(price, 3000.0, 0.01, "reserves→3000");
}

bool test_mid_price_from_reserves_zero() {
  Decimal base{0, 0};
  Decimal quote{3000, 0};
  double price = AmmPoolExtractor::MidPriceFromReserves(base, quote);
  return Check(price == 0.0, "zero base → 0");
}

// --- ResolveMidPrice tests ---

bool test_resolve_mid_price_prefers_sqrt() {
  VenuePoolState ps;
  ps.sqrt_price_x96 = "79228162514264337593543950336";  // price = 1.0
  ps.tick = 100;  // Would give ~1.01

  double price = AmmPoolExtractor::ResolveMidPrice(
      ps, Decimal{0, 0}, Decimal{0, 0});
  return CheckClose(price, 1.0, 0.01, "sqrt preferred over tick");
}

bool test_resolve_mid_price_falls_to_tick() {
  VenuePoolState ps;
  ps.sqrt_price_x96 = "";  // empty
  ps.tick = 100;

  double price = AmmPoolExtractor::ResolveMidPrice(
      ps, Decimal{0, 0}, Decimal{0, 0});
  return CheckClose(price, std::pow(1.0001, 100.0), 0.001,
                    "falls back to tick");
}

bool test_resolve_mid_price_falls_to_reserves() {
  VenuePoolState ps;
  ps.sqrt_price_x96 = "";
  ps.tick = 0;

  Decimal base{100'00000000LL, 8};
  Decimal quote{200'00000000LL, 8};
  double price = AmmPoolExtractor::ResolveMidPrice(ps, base, quote);
  return CheckClose(price, 2.0, 0.01, "falls back to reserves");
}

// --- Extract tests ---

bool test_extract_valid_uniswap_v3() {
  AmmPoolExtractor extractor(DefaultConfig());
  auto fields = MakeFieldsPrice1();
  auto result = extractor.Extract(fields, {});

  if (!Check(result.valid, "valid result")) return false;
  if (!Check(result.pool_state.pool_address == "0xpool1", "pool_address")) return false;
  if (!Check(result.pool_state.liquidity == "1000000", "liquidity")) return false;
  if (!Check(result.pool_state.block_number == 100, "block_number")) return false;
  if (!Check(result.pool_state.finalized, "finalized")) return false;
  if (!CheckClose(result.mid_price, 1.0, 0.01, "mid_price")) return false;
  return true;
}

bool test_extract_with_tick_levels() {
  AmmPoolExtractor extractor(DefaultConfig());
  auto fields = MakeFieldsPrice1();

  std::vector<VenuePoolTickLevel> ticks = {
      {200, Decimal{500, 0}},
      {100, Decimal{1000, 0}},
      {300, Decimal{-300, 0}},
  };

  auto result = extractor.Extract(fields, ticks);

  if (!Check(result.valid, "valid")) return false;
  if (!Check(result.pool_state.ticks.size() == 3, "3 tick levels")) return false;
  // Must be sorted ascending.
  if (!Check(result.pool_state.ticks[0].tick == 100, "tick[0]=100")) return false;
  if (!Check(result.pool_state.ticks[1].tick == 200, "tick[1]=200")) return false;
  if (!Check(result.pool_state.ticks[2].tick == 300, "tick[2]=300")) return false;
  return true;
}

bool test_extract_max_tick_levels() {
  AmmPoolExtractorConfig cfg = DefaultConfig();
  cfg.max_tick_levels = 2;
  AmmPoolExtractor extractor(cfg);
  auto fields = MakeFieldsPrice1();

  std::vector<VenuePoolTickLevel> ticks = {
      {100, Decimal{1, 0}},
      {200, Decimal{2, 0}},
      {300, Decimal{3, 0}},
  };

  auto result = extractor.Extract(fields, ticks);

  if (!Check(result.valid, "valid")) return false;
  if (!Check(result.pool_state.ticks.size() == 2, "trimmed to 2")) return false;
  return true;
}

bool test_extract_with_reserves() {
  AmmPoolExtractor extractor(DefaultConfig());
  std::unordered_map<std::string, std::string> fields = {
      {"reserve_base", "10.0"},
      {"reserve_quote", "30000.0"},
      {"block_number", "50"},
  };

  auto result = extractor.Extract(fields, {});

  if (!Check(result.valid, "valid from reserves")) return false;
  if (!CheckClose(result.mid_price, 3000.0, 1.0, "mid from reserves")) return false;
  return true;
}

bool test_extract_no_price_source() {
  AmmPoolExtractor extractor(DefaultConfig());
  std::unordered_map<std::string, std::string> fields = {
      {"pool_address", "0xnodata"},
      {"block_number", "10"},
  };

  auto result = extractor.Extract(fields, {});

  if (!Check(!result.valid, "invalid: no price source")) return false;
  if (!Check(!result.error.empty(), "has error message")) return false;
  return true;
}

bool test_extract_snake_case_keys() {
  AmmPoolExtractor extractor(DefaultConfig());
  std::unordered_map<std::string, std::string> fields = {
      {"sqrt_price_x96", "79228162514264337593543950336"},
      {"block_number", "42"},
      {"liquidity", "999"},
  };

  auto result = extractor.Extract(fields, {});

  if (!Check(result.valid, "valid with snake_case")) return false;
  if (!CheckClose(result.mid_price, 1.0, 0.01, "mid price")) return false;
  return true;
}

bool test_extract_hex_block_number() {
  AmmPoolExtractor extractor(DefaultConfig());
  std::unordered_map<std::string, std::string> fields = {
      {"sqrtPriceX96", "79228162514264337593543950336"},
      {"block_number", "0x1234"},  // 4660
  };

  auto result = extractor.Extract(fields, {});

  if (!Check(result.valid, "valid with hex block")) return false;
  if (!Check(result.pool_state.block_number == 4660, "hex block = 4660")) return false;
  return true;
}

// --- Cache tests ---

bool test_cache_apply_and_get() {
  AmmPoolExtractor extractor(DefaultConfig());
  auto fields = MakeFieldsPrice1();
  auto result = extractor.Extract(fields, {});

  if (!Check(extractor.ApplyToCache("pool1", result), "apply ok")) return false;

  auto cached = extractor.GetCached("pool1");
  if (!Check(cached.has_value(), "cached present")) return false;
  if (!Check(cached->pool_state.block_number == 100, "cached block_number")) return false;
  return true;
}

bool test_cache_reject_stale_block() {
  AmmPoolExtractor extractor(DefaultConfig());

  auto fields_new = MakeFieldsPrice4();
  auto result_new = extractor.Extract(fields_new, {});
  if (!Check(extractor.ApplyToCache("pool_stale", result_new), "apply block 200")) return false;

  // Now try to apply an older block.
  auto fields_old = MakeFieldsPrice1();  // block 100
  auto result_old = extractor.Extract(fields_old, {});
  if (!Check(!extractor.ApplyToCache("pool_stale", result_old),
             "reject stale block 100 < 200")) return false;

  // Cache should still hold block 200.
  auto cached = extractor.GetCached("pool_stale");
  if (!Check(cached->pool_state.block_number == 200, "still block 200")) return false;
  return true;
}

bool test_cache_reject_duplicate_block() {
  AmmPoolExtractor extractor(DefaultConfig());
  auto fields = MakeFieldsPrice1();
  auto result = extractor.Extract(fields, {});

  if (!Check(extractor.ApplyToCache("pool_dup", result), "first apply")) return false;
  if (!Check(!extractor.ApplyToCache("pool_dup", result), "reject duplicate")) return false;
  return true;
}

bool test_cache_accept_same_block_different_price() {
  AmmPoolExtractor extractor(DefaultConfig());

  auto fields1 = MakeFieldsPrice1();
  auto result1 = extractor.Extract(fields1, {});
  if (!Check(extractor.ApplyToCache("pool_update", result1), "first apply")) return false;

  // Same block but different sqrtPriceX96.
  auto fields2 = MakeFieldsPrice1();
  fields2["sqrtPriceX96"] = "158456325028528675187087900672";  // price=4
  auto result2 = extractor.Extract(fields2, {});
  if (!Check(extractor.ApplyToCache("pool_update", result2),
             "accept same block different price")) return false;
  return true;
}

bool test_cache_monotonic_sequence() {
  AmmPoolExtractor extractor(DefaultConfig());

  auto fields1 = MakeFieldsPrice1();  // block 100
  auto result1 = extractor.Extract(fields1, {});
  extractor.ApplyToCache("pool_seq", result1);

  auto fields2 = MakeFieldsPrice4();  // block 200
  auto result2 = extractor.Extract(fields2, {});
  extractor.ApplyToCache("pool_seq", result2);

  auto cached = extractor.GetCached("pool_seq");
  if (!Check(cached.has_value(), "cached")) return false;
  if (!Check(cached->sequence >= 200, "sequence >= 200")) return false;
  return true;
}

bool test_cache_clear() {
  AmmPoolExtractor extractor(DefaultConfig());
  auto fields = MakeFieldsPrice1();
  auto result = extractor.Extract(fields, {});
  extractor.ApplyToCache("pool_clr", result);

  extractor.ClearCache();

  if (!Check(!extractor.GetCached("pool_clr").has_value(),
             "cleared")) return false;
  return true;
}

bool test_cache_get_nonexistent() {
  AmmPoolExtractor extractor(DefaultConfig());
  if (!Check(!extractor.GetCached("nopool").has_value(),
             "nonexistent returns nullopt")) return false;
  return true;
}

}  // namespace

int main() {
  bool all_passed = true;

  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) {
      std::cerr << "  in test: " << name << std::endl;
      all_passed = false;
    }
  };

  // MidPrice tests
  run("test_mid_price_from_sqrt_price_x96_unit", test_mid_price_from_sqrt_price_x96_unit);
  run("test_mid_price_from_sqrt_price_x96_four", test_mid_price_from_sqrt_price_x96_four);
  run("test_mid_price_from_sqrt_price_x96_empty", test_mid_price_from_sqrt_price_x96_empty);
  run("test_mid_price_from_sqrt_price_x96_zero", test_mid_price_from_sqrt_price_x96_zero);
  run("test_mid_price_from_tick_positive", test_mid_price_from_tick_positive);
  run("test_mid_price_from_tick_negative", test_mid_price_from_tick_negative);
  run("test_mid_price_from_tick_zero", test_mid_price_from_tick_zero);
  run("test_mid_price_from_reserves", test_mid_price_from_reserves);
  run("test_mid_price_from_reserves_zero", test_mid_price_from_reserves_zero);

  // ResolveMidPrice tests
  run("test_resolve_mid_price_prefers_sqrt", test_resolve_mid_price_prefers_sqrt);
  run("test_resolve_mid_price_falls_to_tick", test_resolve_mid_price_falls_to_tick);
  run("test_resolve_mid_price_falls_to_reserves", test_resolve_mid_price_falls_to_reserves);

  // Extract tests
  run("test_extract_valid_uniswap_v3", test_extract_valid_uniswap_v3);
  run("test_extract_with_tick_levels", test_extract_with_tick_levels);
  run("test_extract_max_tick_levels", test_extract_max_tick_levels);
  run("test_extract_with_reserves", test_extract_with_reserves);
  run("test_extract_no_price_source", test_extract_no_price_source);
  run("test_extract_snake_case_keys", test_extract_snake_case_keys);
  run("test_extract_hex_block_number", test_extract_hex_block_number);

  // Cache tests
  run("test_cache_apply_and_get", test_cache_apply_and_get);
  run("test_cache_reject_stale_block", test_cache_reject_stale_block);
  run("test_cache_reject_duplicate_block", test_cache_reject_duplicate_block);
  run("test_cache_accept_same_block_different_price", test_cache_accept_same_block_different_price);
  run("test_cache_monotonic_sequence", test_cache_monotonic_sequence);
  run("test_cache_clear", test_cache_clear);
  run("test_cache_get_nonexistent", test_cache_get_nonexistent);

  if (all_passed) {
    std::cout << "[OK] amm_pool_extractor_test passed (25 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
