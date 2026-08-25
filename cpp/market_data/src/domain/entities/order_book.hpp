#pragma once

#include <chrono>
#include <set>
#include <optional>
#include <cmath>

#include "fill_event.hpp"

namespace cex::market_data::domain {

enum class MarketSource {
  Internal,
  Binance,
  Unknown,
};

struct PriceLevel {
  common::Decimal price;
  common::Decimal quantity;
};

struct BidCompare {
  bool operator()(const PriceLevel& lhs, const PriceLevel& rhs) const {
    return common::Decimal::cmp(lhs.price, rhs.price) > 0;
  }
};

struct AskCompare {
  bool operator()(const PriceLevel& lhs, const PriceLevel& rhs) const {
    return common::Decimal::cmp(lhs.price, rhs.price) < 0;
  }
};

// BBO (Best Bid Offer) structure with all derived metrics
struct BBO {
  double bid_price{0.0};
  double bid_volume{0.0};
  double ask_price{0.0};
  double ask_volume{0.0};
  double mid_price{0.0};
  double spread_absolute{0.0};
  double spread_relative{0.0};  // (ask - bid) / mid * 100
  double microprice{0.0};
  
  bool IsValid() const {
    return bid_price > 0.0 && ask_price > 0.0 && bid_volume > 0.0 && ask_volume > 0.0;
  }
};

struct OrderBook {
  Timestamp timestamp;
  uint64_t nonce;
  MarketSource source;
  std::string symbol;
  std::set<PriceLevel, BidCompare> bid_depth;
  std::set<PriceLevel, AskCompare> ask_depth;

  // background task to update may be necessary
  std::optional<common::Decimal> volume_24h;

  [[nodiscard]] double BestBid() const;
  [[nodiscard]] double BestAsk() const;
  [[nodiscard]] double BestBidVolume() const;
  [[nodiscard]] double BestAskVolume() const;
  [[nodiscard]] double MidPrice() const;
  [[nodiscard]] double Spread() const;
  [[nodiscard]] double SpreadRelative() const;
  [[nodiscard]] double Microprice() const;
  [[nodiscard]] BBO GetBBO() const;

  void Update(const FillEvent& event);
  
  // Call this after any modification to invalidate cache
  void InvalidateCache();

private:
  // Cached values for performance (updated lazily)
  mutable bool cache_valid_{false};
  mutable BBO cached_bbo_;
  
  void RecalculateCache() const;
};

}  // namespace cex::market_data::domain