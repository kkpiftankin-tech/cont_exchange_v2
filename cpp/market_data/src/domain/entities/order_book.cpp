#include "domain/entities/order_book.hpp"
#include <stdexcept>

namespace cex::market_data::domain {

double OrderBook::BestBid() const { 
  if (bid_depth.empty()) return 0.0;
  return static_cast<double>(bid_depth.begin()->price);
}

double OrderBook::BestAsk() const { 
  if (ask_depth.empty()) return 0.0;
  return static_cast<double>(ask_depth.begin()->price);
}

double OrderBook::BestBidVolume() const {
  if (bid_depth.empty()) return 0.0;
  return static_cast<double>(bid_depth.begin()->quantity);
}

double OrderBook::BestAskVolume() const {
  if (ask_depth.empty()) return 0.0;
  return static_cast<double>(ask_depth.begin()->quantity);
}

double OrderBook::MidPrice() const { 
  double bid = BestBid();
  double ask = BestAsk();
  if (bid <= 0.0 || ask <= 0.0) return 0.0;
  return (bid + ask) / 2.0;
}

double OrderBook::Spread() const { 
  double bid = BestBid();
  double ask = BestAsk();
  if (bid <= 0.0 || ask <= 0.0) return 0.0;
  return ask - bid;
}

double OrderBook::SpreadRelative() const {
  double mid = MidPrice();
  double spread = Spread();
  if (mid <= 0.0) return 0.0;
  return (spread / mid) * 100.0;  // Percentage spread
}

double OrderBook::Microprice() const {
  double bid_price = BestBid();
  double ask_price = BestAsk();
  double bid_vol = BestBidVolume();
  double ask_vol = BestAskVolume();
  
  // Edge cases: if one side is empty, return the other side's price
  if (bid_price <= 0.0 || bid_vol <= 0.0) return ask_price;
  if (ask_price <= 0.0 || ask_vol <= 0.0) return bid_price;
  
  // Standard microprice formula: weighted average by volume
  // (bid_vol * ask_price + ask_vol * bid_price) / (bid_vol + ask_vol)
  double numerator = (bid_vol * ask_price) + (ask_vol * bid_price);
  double denominator = bid_vol + ask_vol;
  
  if (denominator <= 0.0) return MidPrice();
  
  return numerator / denominator;
}

void OrderBook::RecalculateCache() const {
  cached_bbo_.bid_price = BestBid();
  cached_bbo_.bid_volume = BestBidVolume();
  cached_bbo_.ask_price = BestAsk();
  cached_bbo_.ask_volume = BestAskVolume();
  cached_bbo_.mid_price = MidPrice();
  cached_bbo_.spread_absolute = Spread();
  cached_bbo_.spread_relative = SpreadRelative();
  cached_bbo_.microprice = Microprice();
  cache_valid_ = true;
}

BBO OrderBook::GetBBO() const {
  if (!cache_valid_) {
    RecalculateCache();
  }
  return cached_bbo_;
}

void OrderBook::InvalidateCache() {
  cache_valid_ = false;
}

void OrderBook::Update(const FillEvent& event) {
  if (event.symbol != symbol) {
    return;
  }

  timestamp = event.timestamp;
  ++nonce;
  
  // Invalidate cache before modification
  InvalidateCache();

  auto apply = [&](auto& depth) {
    using common::Decimal;

    auto it = depth.find(PriceLevel{event.exec_price, 0});
    if (it == depth.end()) {
      return;
    }

    auto new_qty = Decimal::sub(it->quantity, event.exec_qty);
    auto cmp = Decimal::cmp(new_qty, Decimal::zero());
    if (cmp < 0) {
      throw std::invalid_argument("used quantity is bigger than presented");
    }
    depth.erase(it);
    if (cmp > 0) {
      depth.insert(PriceLevel{event.exec_price, new_qty});
    }
  };

  if (event.side == Side::Buy) {
    apply(ask_depth);
  } else {
    apply(bid_depth);
  }
}

}  // namespace cex::market_data::domain