#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "domain/ports/i_liquidity_curve_storage.hpp"

namespace cex::market_data::infra {

// In-memory implementation of liquidity curve storage
class LiquidityCurveMemoryStorage : public domain::ILiquidityCurveStorage {
 public:
  void Store(const fob::venue::v1::VenueLiquidityCurve& curve) override {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string key = curve.venue_id() + "|" + curve.instrument().symbol();
    
    // Store bid curve (BUY side)
    if (curve.has_bid_curve()) {
      curves_[key + "|BUY"] = curve.bid_curve();
    }
    
    // Store ask curve (SELL side)
    if (curve.has_ask_curve()) {
      curves_[key + "|SELL"] = curve.ask_curve();
    }
    
    // Store full metadata
    metadata_[key] = curve;
  }

  std::optional<fob::venue::v1::SideLiquidityCurve> GetCurve(
      const std::string& venue,
      const std::string& symbol,
      fob::venue::v1::ExecutionSide side) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string key = venue + "|" + symbol;
    std::string side_key;
    
    if (side == fob::venue::v1::EXECUTION_SIDE_BUY) {
      side_key = key + "|BUY";
    } else if (side == fob::venue::v1::EXECUTION_SIDE_SELL) {
      side_key = key + "|SELL";
    } else {
      return std::nullopt;
    }
    
    auto it = curves_.find(side_key);
    if (it != curves_.end()) {
      return it->second;
    }
    
    return std::nullopt;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, fob::venue::v1::SideLiquidityCurve> curves_;
  std::unordered_map<std::string, fob::venue::v1::VenueLiquidityCurve> metadata_;
};

}  // namespace cex::market_data::infra
