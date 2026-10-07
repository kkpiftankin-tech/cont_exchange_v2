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
      Level(70010, 50),   // под-лот: СОХРАНЯЕМ сырой объём (не теряем глубину) → +50
      Level(70015, 200),  // rounded up to 70020 for ask side
  };

  const auto asks = CanonicalizeBookSide(raw_asks, BookSide::kAsk, config);
  if (!Check(asks.size() == 2, "ask canonicalization must keep exactly 2 levels")) return false;
  // 70010: 200 (floored 250) + 50 (сырой под-лот) = 250 — глубина сохранена.
  if (!CheckLevel(asks[0], 70010, 250)) return false;
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
      Level(70010, 0),  // qty<=0 — единственный уровень пустой ⇒ сторона пуста
  };

  const auto canonical = CanonicalizeOrderBook(raw_bids, raw_asks, config);
  if (!Check(canonical.bids.size() == 1, "bid side must stay non-empty")) return false;
  if (!Check(canonical.asks.empty(), "ask side must become empty")) return false;
  if (!Check(canonical.is_empty(), "order book with missing side must be marked empty")) return false;
  return true;
}

bool TestCoarseTickPreservesDepthForLowPricedPair() {
  // ETH/BTC-подобная книга: цена ~0.032 (scale 5), но venue-wide tick 0.01.
  // Без guard'а floor/ceil к тику схлопнул бы все уровни в один бакет (0.03);
  // guard (best price < 100·tick ⇒ тик отключается) сохраняет глубину книги.
  const DepthCanonicalizationConfig config{
      .tick_size = Decimal{.units = 1, .scale = 2},  // 0.01 — грубый для цены ~0.032
      .lot_size = Decimal{.units = 0, .scale = 0},
      .min_qty = Decimal{.units = 0, .scale = 0},
      .max_levels_per_side = 0,
  };

  const std::vector<BookLevel> raw_bids = {
      Level(3217, 500, 5),  // 0.03217
      Level(3216, 600, 5),  // 0.03216
      Level(3215, 700, 5),  // 0.03215
  };

  const auto bids = CanonicalizeBookSide(raw_bids, BookSide::kBid, config);
  if (!Check(bids.size() == 3, "coarse tick must NOT collapse low-priced book depth")) return false;
  if (!CheckLevel(bids[0], 3217, 500, 5)) return false;
  if (!CheckLevel(bids[1], 3216, 600, 5)) return false;
  if (!CheckLevel(bids[2], 3215, 700, 5)) return false;
  return true;
}

bool TestSubLotDepthPreservedForHighPricedPair() {
  // BTC/USDT-подобная книга: цена ~86096 (тик 0.01), объёмы уровней крошечные
  // (0.00006 BTC ≈ $5) при лоте 0.001. Старое поведение (floor к лоту → 0 → drop)
  // стирало ~половину реальной глубины (binance отдаёт 20 → оставалось 5-15).
  // Теперь под-лотовые уровни СОХРАНЯЮТСЯ с сырым объёмом — стакан отражает действительность.
  const DepthCanonicalizationConfig config{
      .tick_size = Decimal{.units = 1, .scale = 2},  // 0.01
      .lot_size = Decimal{.units = 1, .scale = 3},   // 0.001
      .min_qty = Decimal{.units = 0, .scale = 0},
      .max_levels_per_side = 0,
  };
  const std::vector<BookLevel> raw_bids = {
      Level(8609600, 6000, 2, 8),    // 86096.00, qty 0.00006 (под-лот)
      Level(8609599, 200000, 2, 8),  // 86095.99, qty 0.002 (кратно лоту)
      Level(8609598, 12000, 2, 8),   // 86095.98, qty 0.00012 (под-лот)
  };
  const auto bids = CanonicalizeBookSide(raw_bids, BookSide::kBid, config);
  if (!Check(bids.size() == 3, "под-лотовые уровни BTC не должны выбрасываться")) return false;
  if (!CheckLevel(bids[0], 8609600, 6000, 2, 8)) return false;    // dust сохранён сырым
  if (!CheckLevel(bids[1], 8609599, 200000, 2, 8)) return false;  // ≥лот (floor no-op)
  if (!CheckLevel(bids[2], 8609598, 12000, 2, 8)) return false;   // dust сохранён
  return true;
}

}  // namespace

int main() {
  if (!TestBidSideCanonicalization()) return EXIT_FAILURE;
  if (!TestAskSideCanonicalization()) return EXIT_FAILURE;
  if (!TestMinQtyAndTopN()) return EXIT_FAILURE;
  if (!TestOrderBookEmptyStatus()) return EXIT_FAILURE;
  if (!TestCoarseTickPreservesDepthForLowPricedPair()) return EXIT_FAILURE;
  if (!TestSubLotDepthPreservedForHighPricedPair()) return EXIT_FAILURE;

  std::cout << "[OK] depth_canonicalizer_test passed" << std::endl;
  return EXIT_SUCCESS;
}
