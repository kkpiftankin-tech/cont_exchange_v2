#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "domain/amm_pool_extractor.hpp"
#include "domain/amm_virtual_lob.hpp"

namespace {

using cex::common::Decimal;
using cex::venues::domain::AmmPoolExtractResult;
using cex::venues::domain::AmmPoolExtractor;
using cex::venues::domain::AmmPoolExtractorConfig;
using cex::venues::domain::BuildTickBasedSide;
using cex::venues::domain::BuildUniformSide;
using cex::venues::domain::BuildVirtualLOBFromAMM;
using cex::venues::domain::VenueBookLevel;
using cex::venues::domain::VenuePoolState;
using cex::venues::domain::VenuePoolTickLevel;
using cex::venues::domain::VirtualLob;
using cex::venues::domain::VirtualLobConfig;

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

VirtualLobConfig DefaultConfig() {
  return VirtualLobConfig{
      .levels_per_side = 10,
      .price_scale = 8,
      .qty_scale = 8,
      .fee_rate = 0.003,
      .tick_spacing = 60,
  };
}

AmmPoolExtractResult MakePoolWithTicks() {
  // Pool at price ~1.0 (tick=0), with liquidity and initialized ticks.
  AmmPoolExtractResult result;
  result.valid = true;
  result.mid_price = 1.0;

  result.pool_state.sqrt_price_x96 = "79228162514264337593543950336";
  result.pool_state.tick = 0;
  result.pool_state.liquidity = "1000000000";
  result.pool_state.block_number = 100;

  // Create ticks spanning around tick 0.
  // Ticks: -600, -300, 0, 300, 600
  result.pool_state.ticks = {
      VenuePoolTickLevel{-600, Decimal{500000, 0}},
      VenuePoolTickLevel{-300, Decimal{800000, 0}},
      VenuePoolTickLevel{0, Decimal{-200000, 0}},
      VenuePoolTickLevel{300, Decimal{-600000, 0}},
      VenuePoolTickLevel{600, Decimal{-500000, 0}},
  };

  result.reserve_base = Decimal{100'00000000LL, 8};
  result.reserve_quote = Decimal{100'00000000LL, 8};
  result.sequence = 100;
  return result;
}

AmmPoolExtractResult MakePoolNoTicks() {
  AmmPoolExtractResult result;
  result.valid = true;
  result.mid_price = 3000.0;
  result.pool_state.tick = 0;
  result.pool_state.block_number = 50;
  result.reserve_base = Decimal{10'00000000, 8};  // 10.0
  result.reserve_quote = Decimal{30000'00000000LL, 8};  // 30000.0
  result.sequence = 50;
  return result;
}

AmmPoolExtractResult MakeInvalidPool() {
  AmmPoolExtractResult result;
  result.valid = false;
  result.mid_price = 0.0;
  return result;
}

// --- BuildUniformSide tests ---

bool test_uniform_bids_basic() {
  auto bids = BuildUniformSide(1000.0, 0.003, 5.0, 8, 8, 5, true);
  if (!Check(!bids.empty(), "bids not empty")) return false;
  if (!Check(bids.size() == 5, "5 bid levels")) return false;

  // Bids must be sorted descending by price.
  for (std::size_t i = 1; i < bids.size(); ++i) {
    if (!Check(bids[i].price.units < bids[i - 1].price.units,
               "bids descending")) return false;
  }

  // First bid should be near mid * (1 - fee).
  double first_price = static_cast<double>(bids[0].price);
  if (!CheckClose(first_price, 997.0, 2.0, "first bid ~997")) return false;
  return true;
}

bool test_uniform_asks_basic() {
  auto asks = BuildUniformSide(1000.0, 0.003, 5.0, 8, 8, 5, false);
  if (!Check(!asks.empty(), "asks not empty")) return false;
  if (!Check(asks.size() == 5, "5 ask levels")) return false;

  // Asks must be sorted ascending by price.
  for (std::size_t i = 1; i < asks.size(); ++i) {
    if (!Check(asks[i].price.units > asks[i - 1].price.units,
               "asks ascending")) return false;
  }

  double first_price = static_cast<double>(asks[0].price);
  if (!CheckClose(first_price, 1003.0, 2.0, "first ask ~1003")) return false;
  return true;
}

bool test_uniform_zero_mid() {
  auto bids = BuildUniformSide(0.0, 0.003, 1.0, 8, 8, 5, true);
  // Should still produce levels (mid clamped to min step).
  if (!Check(!bids.empty(), "non-empty even with mid=0")) return false;
  return true;
}

bool test_uniform_zero_liquidity() {
  auto bids = BuildUniformSide(1000.0, 0.003, 0.0, 8, 8, 5, true);
  // Should still produce levels (fallback qty).
  if (!Check(!bids.empty(), "non-empty with zero liquidity")) return false;
  return true;
}

bool test_uniform_positive_qty() {
  auto bids = BuildUniformSide(1000.0, 0.003, 5.0, 8, 8, 5, true);
  for (const auto& level : bids) {
    if (!Check(level.qty.units > 0, "positive qty")) return false;
  }
  return true;
}

// --- BuildTickBasedSide tests ---

bool test_tick_based_bids() {
  auto pool = MakePoolWithTicks();
  auto bids = BuildTickBasedSide(
      pool.pool_state, pool.mid_price, 0.003, 8, 8, 10, true);

  // Should produce some bid levels from ticks below current tick.
  if (!Check(!bids.empty(), "tick-based bids not empty")) return false;

  // Bids sorted descending.
  for (std::size_t i = 1; i < bids.size(); ++i) {
    if (!Check(bids[i].price.units <= bids[i - 1].price.units,
               "tick bids descending")) return false;
  }
  return true;
}

bool test_tick_based_asks() {
  auto pool = MakePoolWithTicks();
  auto asks = BuildTickBasedSide(
      pool.pool_state, pool.mid_price, 0.003, 8, 8, 10, false);

  // Should produce some ask levels from ticks above current tick.
  if (!Check(!asks.empty(), "tick-based asks not empty")) return false;

  // Asks sorted ascending.
  for (std::size_t i = 1; i < asks.size(); ++i) {
    if (!Check(asks[i].price.units >= asks[i - 1].price.units,
               "tick asks ascending")) return false;
  }
  return true;
}

bool test_tick_based_empty_ticks() {
  VenuePoolState ps;
  ps.tick = 0;
  ps.liquidity = "1000";
  // No ticks.

  auto bids = BuildTickBasedSide(ps, 1.0, 0.003, 8, 8, 5, true);
  if (!Check(bids.empty(), "empty ticks → empty bids")) return false;
  return true;
}

bool test_tick_based_zero_liquidity() {
  VenuePoolState ps;
  ps.tick = 0;
  ps.liquidity = "0";
  ps.ticks = {
      VenuePoolTickLevel{-300, Decimal{1000, 0}},
      VenuePoolTickLevel{-100, Decimal{-1000, 0}},
  };

  auto bids = BuildTickBasedSide(ps, 1.0, 0.003, 8, 8, 5, true);
  // May produce levels if liquidityNet compensates, or empty.
  // Either way, no crash.
  (void)bids;
  return true;
}

bool test_tick_based_max_levels() {
  auto pool = MakePoolWithTicks();
  auto bids = BuildTickBasedSide(
      pool.pool_state, pool.mid_price, 0.003, 8, 8, 2, true);
  if (!Check(bids.size() <= 2, "max 2 levels")) return false;
  return true;
}

// --- BuildVirtualLOBFromAMM tests ---

bool test_build_lob_with_ticks() {
  auto pool = MakePoolWithTicks();
  auto cfg = DefaultConfig();
  auto lob = BuildVirtualLOBFromAMM(pool, cfg);

  if (!Check(!lob.empty(), "LOB not empty")) return false;
  if (!CheckClose(lob.mid_price, 1.0, 0.01, "mid_price=1.0")) return false;
  if (!Check(!lob.bids.empty(), "has bids")) return false;
  if (!Check(!lob.asks.empty(), "has asks")) return false;
  return true;
}

bool test_build_lob_no_ticks_uniform_fallback() {
  auto pool = MakePoolNoTicks();
  auto cfg = DefaultConfig();
  auto lob = BuildVirtualLOBFromAMM(pool, cfg);

  if (!Check(!lob.empty(), "LOB not empty (uniform)")) return false;
  if (!CheckClose(lob.mid_price, 3000.0, 1.0, "mid=3000")) return false;

  // Bids should be below mid, asks above.
  double first_bid = static_cast<double>(lob.bids[0].price);
  double first_ask = static_cast<double>(lob.asks[0].price);
  if (!Check(first_bid < lob.mid_price, "bid < mid")) return false;
  if (!Check(first_ask > lob.mid_price, "ask > mid")) return false;
  return true;
}

bool test_build_lob_invalid_pool() {
  auto pool = MakeInvalidPool();
  auto cfg = DefaultConfig();
  auto lob = BuildVirtualLOBFromAMM(pool, cfg);

  if (!Check(lob.empty(), "invalid pool → empty LOB")) return false;
  return true;
}

bool test_build_lob_zero_mid() {
  AmmPoolExtractResult pool;
  pool.valid = true;
  pool.mid_price = 0.0;

  auto lob = BuildVirtualLOBFromAMM(pool, DefaultConfig());
  if (!Check(lob.empty(), "zero mid → empty LOB")) return false;
  return true;
}

bool test_build_lob_levels_count() {
  auto pool = MakePoolNoTicks();
  VirtualLobConfig cfg = DefaultConfig();
  cfg.levels_per_side = 5;

  auto lob = BuildVirtualLOBFromAMM(pool, cfg);
  if (!Check(lob.bids.size() == 5, "5 bid levels")) return false;
  if (!Check(lob.asks.size() == 5, "5 ask levels")) return false;
  return true;
}

bool test_build_lob_bids_asks_dont_cross() {
  auto pool = MakePoolNoTicks();
  auto cfg = DefaultConfig();
  auto lob = BuildVirtualLOBFromAMM(pool, cfg);

  if (!Check(!lob.empty(), "not empty")) return false;

  double best_bid = static_cast<double>(lob.bids[0].price);
  double best_ask = static_cast<double>(lob.asks[0].price);
  if (!Check(best_bid < best_ask, "bid < ask (no crossing)")) return false;
  return true;
}

bool test_build_lob_tick_fallback_to_uniform() {
  // Pool claims to have ticks, but they produce empty levels.
  AmmPoolExtractResult pool;
  pool.valid = true;
  pool.mid_price = 100.0;
  pool.pool_state.tick = 0;
  pool.pool_state.liquidity = "0";  // zero liquidity
  pool.pool_state.ticks = {
      VenuePoolTickLevel{-100, Decimal{0, 0}},
      VenuePoolTickLevel{100, Decimal{0, 0}},
  };
  pool.reserve_base = Decimal{50'00000000LL, 8};
  pool.reserve_quote = Decimal{5000'00000000LL, 8};

  auto lob = BuildVirtualLOBFromAMM(pool, DefaultConfig());
  // Should fallback to uniform since tick-based produces no qty.
  if (!Check(!lob.empty(), "fallback to uniform")) return false;
  return true;
}

bool test_build_lob_positive_quantities() {
  auto pool = MakePoolNoTicks();
  auto lob = BuildVirtualLOBFromAMM(pool, DefaultConfig());

  for (const auto& level : lob.bids) {
    if (!Check(level.qty.units > 0, "bid qty > 0")) return false;
    if (!Check(level.price.units > 0, "bid price > 0")) return false;
  }
  for (const auto& level : lob.asks) {
    if (!Check(level.qty.units > 0, "ask qty > 0")) return false;
    if (!Check(level.price.units > 0, "ask price > 0")) return false;
  }
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

  // Uniform side tests
  run("test_uniform_bids_basic", test_uniform_bids_basic);
  run("test_uniform_asks_basic", test_uniform_asks_basic);
  run("test_uniform_zero_mid", test_uniform_zero_mid);
  run("test_uniform_zero_liquidity", test_uniform_zero_liquidity);
  run("test_uniform_positive_qty", test_uniform_positive_qty);

  // Tick-based side tests
  run("test_tick_based_bids", test_tick_based_bids);
  run("test_tick_based_asks", test_tick_based_asks);
  run("test_tick_based_empty_ticks", test_tick_based_empty_ticks);
  run("test_tick_based_zero_liquidity", test_tick_based_zero_liquidity);
  run("test_tick_based_max_levels", test_tick_based_max_levels);

  // Full BuildVirtualLOBFromAMM tests
  run("test_build_lob_with_ticks", test_build_lob_with_ticks);
  run("test_build_lob_no_ticks_uniform_fallback", test_build_lob_no_ticks_uniform_fallback);
  run("test_build_lob_invalid_pool", test_build_lob_invalid_pool);
  run("test_build_lob_zero_mid", test_build_lob_zero_mid);
  run("test_build_lob_levels_count", test_build_lob_levels_count);
  run("test_build_lob_bids_asks_dont_cross", test_build_lob_bids_asks_dont_cross);
  run("test_build_lob_tick_fallback_to_uniform", test_build_lob_tick_fallback_to_uniform);
  run("test_build_lob_positive_quantities", test_build_lob_positive_quantities);

  if (all_passed) {
    std::cout << "[OK] amm_virtual_lob_test passed (18 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
