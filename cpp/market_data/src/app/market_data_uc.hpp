#pragma once
// =============================================================================
// MarketDataUseCases — потокобезопасный кэш "последнего тикера" по
// (venue, symbol). Обновляется из Kafka (marketdata.raw), читается из gRPC.
//
// Это MVP-вариант: одна актуальная строчка на пару, без истории и без полного
// orderbook. Полноценный market data сервис должен предоставлять ещё и
// аггрегаты (best bid/ask, OHLCV, свечи), стрим-подписку и снапшоты.
// =============================================================================

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "fob/marketdata/v1/marketdata_raw.pb.h"

namespace cex::market_data::app {

class MarketDataUseCases {
 public:
  // Принять "сырое" событие из Kafka. Если внутри пришёл Ticker — обновляем кэш.
  // Другие типы событий (трейды, ордербук) пока игнорируются.
  void OnMarketDataRaw(const fob::marketdata::v1::MarketDataRaw& evt);

  // Получить последний тикер по venue/symbol.
  // nullopt — данных ещё не приходило (например, venue только подключилась).
  std::optional<fob::marketdata::v1::Ticker> GetLastTicker(
      const std::string& venue, const std::string& symbol) const;

 private:
  // Композитный ключ "venue|symbol" — простой и достаточный для in-memory map.
  static std::string key(const std::string& venue, const std::string& symbol);

  // mutable — чтобы const-методы (GetLastTicker) могли захватывать lock.
  mutable std::mutex mu_;
  std::unordered_map<std::string, fob::marketdata::v1::Ticker> last_ticker_;
};

}  // namespace cex::market_data::app
