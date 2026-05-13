// =============================================================================
// Реализация MarketDataUseCases — простой кэш на map'е под мьютексом.
// =============================================================================

#include "app/market_data_uc.hpp"

#include "cex/common/log.hpp"

namespace cex::market_data::app {

// Композитный ключ. Используем символ '|', который точно не встречается ни
// в venue (alphanumeric), ни в символе пары — иначе возможна коллизия.
std::string MarketDataUseCases::key(const std::string& venue, const std::string& symbol) {
  return venue + "|" + symbol;
}

// На входе MarketDataRaw — это oneof из тикера/трейда/ордербука и т.п.
// В MVP интересует только тикер; всё остальное молча игнорируем.
void MarketDataUseCases::OnMarketDataRaw(const fob::marketdata::v1::MarketDataRaw& evt) {
  if (!evt.has_ticker()) return;
  std::lock_guard<std::mutex> lg(mu_);
  const auto& t = evt.ticker();
  // Перезаписываем целиком: каждый новый тикер — это полная запись.
  last_ticker_[key(t.venue(), t.instrument().symbol())] = t;
}

std::optional<fob::marketdata::v1::Ticker> MarketDataUseCases::GetLastTicker(
    const std::string& venue, const std::string& symbol) const {
  std::lock_guard<std::mutex> lg(mu_);
  auto it = last_ticker_.find(key(venue, symbol));
  if (it == last_ticker_.end()) return std::nullopt;
  return it->second; // копия — отдаём вызвавшему вне локa
}

}  // namespace cex::market_data::app
