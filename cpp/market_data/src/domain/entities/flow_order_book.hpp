#pragma once

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/entities/market_data_snapshot.hpp"

namespace cex::market_data::domain {

// FOB aggregate curves — bestBid/bestAsk из активных FlowOrder.
//
// По спецификации F-05:
//   bestBid = max(price_high) среди активных BUY-ордеров
//             (цена отрыва aggregate demand от нуля)
//   bestAsk = min(price_low)  среди активных SELL-ордеров
//             (цена отрыва aggregate supply от нуля)
//
// Thread-safe.
class FlowOrderBook {
 public:
  struct OrderEntry {
    common::Decimal price_low;
    common::Decimal price_high;
    common::Decimal max_speed;
    bool is_buy{true};
  };

  void AddOrder(const std::string& order_id, const std::string& asset,
                const OrderEntry& entry) {
    std::lock_guard<std::mutex> lg(mu_);
    orders_[asset][order_id] = entry;
    order_asset_index_[order_id] = asset;
  }

  void RemoveOrder(const std::string& order_id, const std::string& asset) {
    std::lock_guard<std::mutex> lg(mu_);
    auto it = orders_.find(asset);
    if (it != orders_.end()) {
      it->second.erase(order_id);
    }
    order_asset_index_.erase(order_id);
  }

  // Удалить по order_id без знания asset (для FlowOrderCancel)
  void RemoveOrderById(const std::string& order_id) {
    std::lock_guard<std::mutex> lg(mu_);
    auto idx_it = order_asset_index_.find(order_id);
    if (idx_it == order_asset_index_.end()) return;
    const std::string& asset = idx_it->second;
    auto it = orders_.find(asset);
    if (it != orders_.end()) it->second.erase(order_id);
    order_asset_index_.erase(idx_it);
  }

  // bestBid = max(price_high) среди BUY-ордеров для asset
  std::optional<common::Decimal> BestBid(const std::string& asset) const {
    std::lock_guard<std::mutex> lg(mu_);
    auto it = orders_.find(asset);
    if (it == orders_.end() || it->second.empty()) return std::nullopt;

    std::optional<common::Decimal> best;
    for (const auto& [oid, e] : it->second) {
      if (!e.is_buy) continue;
      if (!best.has_value() || common::Decimal::cmp(e.price_high, *best) > 0) {
        best = e.price_high;
      }
    }
    return best;
  }

  // bestAsk = min(price_low) среди SELL-ордеров для asset
  std::optional<common::Decimal> BestAsk(const std::string& asset) const {
    std::lock_guard<std::mutex> lg(mu_);
    auto it = orders_.find(asset);
    if (it == orders_.end() || it->second.empty()) return std::nullopt;

    std::optional<common::Decimal> best;
    for (const auto& [oid, e] : it->second) {
      if (e.is_buy) continue;
      if (!best.has_value() || common::Decimal::cmp(e.price_low, *best) < 0) {
        best = e.price_low;
      }
    }
    return best;
  }

  bool HasOrders(const std::string& asset) const {
    std::lock_guard<std::mutex> lg(mu_);
    auto it = orders_.find(asset);
    return it != orders_.end() && !it->second.empty();
  }

  // F5-6: bidDepth/askDepth — суммарный поток (qRate = max_speed) активных
  // FlowOrder, агрегированный по ценовым уровням. Цена уровня для BUY-ордера —
  // его price_high (лучшая цена покупки), для SELL — price_low. Уровни
  // сортируются: bid по убыванию, ask по возрастанию, обрезаются до `levels`.
  std::vector<DepthLevel> DepthLevels(const std::string& asset, bool bid_side,
                                      uint32_t levels) const {
    std::lock_guard<std::mutex> lg(mu_);
    std::vector<DepthLevel> result;
    auto it = orders_.find(asset);
    if (it == orders_.end() || it->second.empty()) return result;

    // Агрегируем max_speed по равным ценовым уровням.
    std::vector<std::pair<common::Decimal, common::Decimal>> agg;
    for (const auto& [oid, e] : it->second) {
      if (e.is_buy != bid_side) continue;
      const common::Decimal& price = bid_side ? e.price_high : e.price_low;
      bool merged = false;
      for (auto& lvl : agg) {
        if (common::Decimal::cmp(lvl.first, price) == 0) {
          lvl.second = common::Decimal::add(lvl.second, e.max_speed);
          merged = true;
          break;
        }
      }
      if (!merged) agg.emplace_back(price, e.max_speed);
    }

    std::sort(agg.begin(), agg.end(),
              [bid_side](const auto& a, const auto& b) {
                const int c = common::Decimal::cmp(a.first, b.first);
                return bid_side ? (c > 0) : (c < 0);
              });

    for (const auto& lvl : agg) {
      if (result.size() >= levels) break;
      result.push_back({lvl.first, lvl.second});
    }
    return result;
  }

 private:
  mutable std::mutex mu_;
  // asset → (order_id → entry)
  std::unordered_map<std::string,
      std::unordered_map<std::string, OrderEntry>> orders_;
  // order_id → asset (для RemoveOrderById без знания asset)
  std::unordered_map<std::string, std::string> order_asset_index_;
};

}  // namespace cex::market_data::domain
