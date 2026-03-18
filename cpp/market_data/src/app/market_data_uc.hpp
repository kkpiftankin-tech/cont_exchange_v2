#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "fob/marketdata/v1/marketdata_raw.pb.h"

namespace cex::market_data::app {

// In-memory latest-ticker store.
class MarketDataUseCases {
 public:
  void OnMarketDataRaw(const fob::marketdata::v1::MarketDataRaw& evt);

  std::optional<fob::marketdata::v1::Ticker> GetLastTicker(
      const std::string& venue, const std::string& symbol) const;

 private:
  static std::string key(const std::string& venue, const std::string& symbol);

  mutable std::mutex mu_;
  std::unordered_map<std::string, fob::marketdata::v1::Ticker> last_ticker_;
};

}  // namespace cex::market_data::app
