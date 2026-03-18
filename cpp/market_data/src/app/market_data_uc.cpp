#include "app/market_data_uc.hpp"

#include "cex/common/log.hpp"

namespace cex::market_data::app {

std::string MarketDataUseCases::key(const std::string& venue, const std::string& symbol) {
  return venue + "|" + symbol;
}

void MarketDataUseCases::OnMarketDataRaw(const fob::marketdata::v1::MarketDataRaw& evt) {
  if (!evt.has_ticker()) return;
  std::lock_guard<std::mutex> lg(mu_);
  const auto& t = evt.ticker();
  last_ticker_[key(t.venue(), t.instrument().symbol())] = t;
}

std::optional<fob::marketdata::v1::Ticker> MarketDataUseCases::GetLastTicker(
    const std::string& venue, const std::string& symbol) const {
  std::lock_guard<std::mutex> lg(mu_);
  auto it = last_ticker_.find(key(venue, symbol));
  if (it == last_ticker_.end()) return std::nullopt;
  return it->second;
}

}  // namespace cex::market_data::app
