#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "domain/depth_canonicalizer.hpp"

namespace {

using cex::common::Decimal;
using cex::venues::domain::BookLevel;
using cex::venues::domain::BookSide;
using cex::venues::domain::CanonicalizeBookSide;
using cex::venues::domain::CanonicalizeOrderBook;
using cex::venues::domain::DepthCanonicalizationConfig;

BookLevel Level(const int64_t price_units,
                const int64_t qty_units,
                const int32_t price_scale = 0,
                const int32_t qty_scale = 3) {
  return BookLevel{
      .price = Decimal{.units = price_units, .scale = price_scale},
      .qty = Decimal{.units = qty_units, .scale = qty_scale},
  };
}

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool CheckLevel(const BookLevel& level,
                const int64_t expected_price_units,
                const int64_t expected_qty_units,
                const int32_t expected_price_scale = 0,
                const int32_t expected_qty_scale = 3) {
  if (!Check(level.price.units == expected_price_units, "unexpected level.price.units")) return false;
  if (!Check(level.price.scale == expected_price_scale, "unexpected level.price.scale")) return false;
  if (!Check(level.qty.units == expected_qty_units, "unexpected level.qty.units")) return false;
  if (!Check(level.qty.scale == expected_qty_scale, "unexpected level.qty.scale")) return false;
  return true;
}

bool TestBidSideCanonicalization() {
  const DepthCanonicalizationConfig config{
      .tick_size = Decimal{.units = 10, .scale = 0},
      .lot_size = Decimal{.units = 100, .scale = 3},  // 0.100
      .min_qty = Decimal{.units = 0, .scale = 0},
      .max_levels_per_side = 0,
  };

  const std::vector<BookLevel> raw_bids = {
      Level(70000, 200),  // 0.2
      Level(69990, 0),    // filtered (qty <= 0)
      Level(70000, 300),  // same price -> must be aggregated
      Level(70010, 100),
      Level(69995, 200),  // rounded down to 69990 for bid side
  };

  const auto bids = CanonicalizeBookSide(raw_bids, BookSide::kBid, config);
  if (!Check(bids.size() == 3, "bid canonicalization must keep exactly 3 levels")) return false;
  if (!CheckLevel(bids[0], 70010, 100)) return false;
  if (!CheckLevel(bids[1], 70000, 500)) return false;
  if (!CheckLevel(bids[2], 69990, 200)) return false;
  return true;
}

bool TestAskSideCanonicalization() {
  const DepthCanonicalizationConfig config{
      .tick_size = Decimal{.units = 10, .scale = 0},
      .lot_size = Decimal{.units = 100, .scale = 3},  // 0.100
      .min_qty = Decimal{.units = 0, .scale = 0},
      .max_levels_per_side = 0,
  };

  const std::vector<BookLevel> raw_asks = {
      Level(70020, 200),
      Level(70010, 250),  // rounded to 0.2 by lot
      Level(70010, 50),   // rounded to 0 and removed
      Level(70015, 200),  // rounded up to 70020 for ask side
  };

  const auto asks = CanonicalizeBookSide(raw_asks, BookSide::kAsk, config);
  if (!Check(asks.size() == 2, "ask canonicalization must keep exactly 2 levels")) return false;
  if (!CheckLevel(asks[0], 70010, 200)) return false;
  if (!CheckLevel(asks[1], 70020, 400)) return false;
  return true;
}

bool TestMinQtyAndTopN() {
  const DepthCanonicalizationConfig config{
      .tick_size = Decimal{.units = 10, .scale = 0},
      .lot_size = Decimal{.units = 100, .scale = 3},  // 0.100
      .min_qty = Decimal{.units = 300, .scale = 3},   // 0.300
      .max_levels_per_side = 2,
  };

  const std::vector<BookLevel> raw_bids = {
      Level(70000, 350),  // 0.3 after lot -> keep
      Level(69990, 250),  // 0.2 after lot -> drop by min_qty
      Level(70010, 900),  // keep
      Level(70020, 450),  // 0.4 after lot -> keep
  };

  const auto bids = CanonicalizeBookSide(raw_bids, BookSide::kBid, config);
  if (!Check(bids.size() == 2, "top-N trimming must keep 2 best bid levels")) return false;
  if (!CheckLevel(bids[0], 70020, 400)) return false;
  if (!CheckLevel(bids[1], 70010, 900)) return false;
  return true;
}

bool TestOrderBookEmptyStatus() {
  const DepthCanonicalizationConfig config{
      .tick_size = Decimal{.units = 10, .scale = 0},
      .lot_size = Decimal{.units = 100, .scale = 3},  // 0.100
      .min_qty = Decimal{.units = 0, .scale = 0},
      .max_levels_per_side = 0,
  };

  const std::vector<BookLevel> raw_bids = {Level(70000, 200)};
  const std::vector<BookLevel> raw_asks = {
      Level(70010, 50),  // rounded to 0, removed
  };

  const auto canonical = CanonicalizeOrderBook(raw_bids, raw_asks, config);
  if (!Check(canonical.bids.size() == 1, "bid side must stay non-empty")) return false;
  if (!Check(canonical.asks.empty(), "ask side must become empty")) return false;
  if (!Check(canonical.is_empty(), "order book with missing side must be marked empty")) return false;
  return true;
}

}  // namespace

int main() {
  if (!TestBidSideCanonicalization()) return EXIT_FAILURE;
  if (!TestAskSideCanonicalization()) return EXIT_FAILURE;
  if (!TestMinQtyAndTopN()) return EXIT_FAILURE;
  if (!TestOrderBookEmptyStatus()) return EXIT_FAILURE;

  std::cout << "[OK] depth_canonicalizer_test passed" << std::endl;
  return EXIT_SUCCESS;
}
