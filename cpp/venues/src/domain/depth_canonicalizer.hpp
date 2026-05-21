#pragma once

#include <cstddef>
#include <vector>

#include "cex/common/decimal.hpp"

namespace cex::venues::domain {

enum class BookSide {
  kBid,
  kAsk,
};

struct BookLevel {
  cex::common::Decimal price;
  cex::common::Decimal qty;
};

struct DepthCanonicalizationConfig {
  cex::common::Decimal tick_size{0, 0};
  cex::common::Decimal lot_size{0, 0};
  cex::common::Decimal min_qty{0, 0};

  // 0 means "do not trim".
  std::size_t max_levels_per_side{0};
};

struct CanonicalOrderBook {
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;

  bool is_empty() const {
    return bids.empty() || asks.empty();
  }
};

// Canonicalization rules:
// - remove invalid/non-positive levels;
// - quantize by tick/lot;
// - sort (bid desc, ask asc);
// - aggregate equal prices;
// - filter by min_qty and trim top-N levels.
std::vector<BookLevel> CanonicalizeBookSide(
    const std::vector<BookLevel>& levels,
    BookSide side,
    const DepthCanonicalizationConfig& config);

CanonicalOrderBook CanonicalizeOrderBook(
    const std::vector<BookLevel>& bids,
    const std::vector<BookLevel>& asks,
    const DepthCanonicalizationConfig& config);

}  // namespace cex::venues::domain
