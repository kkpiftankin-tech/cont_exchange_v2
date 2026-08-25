#pragma once

#include <optional>
#include <string>
#include <vector>

#include "fob/matching/v1/batch.pb.h"
#include "fob/venue/v1/venue.pb.h"

#include "domain/entities/market_data_snapshot.hpp"
#include "domain/entities/order_book.hpp"

namespace cex::market_data::app {

struct MarketDataConfig {
  uint32_t depth_levels{10};
  double   spread_alert_threshold_bps{50.0};
  uint32_t stale_threshold_sec{30};
  uint32_t volume_window_sec{86400};
};

// Вычисляет MarketDataSnapshot из BatchResult + текущего OrderBook.
//
// Алгоритм (MVP-симуляция FOB aggregate curves):
//   1. clear_price из BatchResult → primary price signal
//   2. bestBid/bestAsk: из OrderBook (venue или агрегированный), либо оценка от clear_price
//   3. mid = (bestBid + bestAsk) / 2
//   4. spread, spreadBps
//   5. depth: N уровней из OrderBook
//   6. volume24h: сумма execQty из fills в текущем BatchResult (полное окно 24h — в ClickHouse)
//
// В production FOB bestBid/bestAsk определяются из aggregate demand/supply curves
// по findDetachPrice. Для MVP используется order book.
class ComputeMarketData {
 public:
  // Основной расчёт при поступлении BatchResult.
  // fob_best_bid / fob_best_ask — из FOB FlowOrder aggregate curves (F5-3).
  // Если nullopt — используется venue OrderBook (fallback MVP).
  static std::optional<domain::MarketDataSnapshot> FromBatchResult(
      const fob::matching::v1::BatchResult& batch,
      const std::string& asset,
      const std::optional<domain::OrderBook>& current_book,
      const MarketDataConfig& cfg,
      std::optional<common::Decimal> fob_best_bid = std::nullopt,
      std::optional<common::Decimal> fob_best_ask = std::nullopt);

  // Обновление composite-метрик при поступлении VenueSnapshot (внешние котировки).
  // Используется когда нет внутренних данных (пустой рынок).
  static std::optional<domain::MarketDataSnapshot> FromVenueSnapshot(
      const fob::venue::v1::VenueSnapshot& snapshot,
      const std::string& asset,
      const MarketDataConfig& cfg);

 private:
  static common::Decimal ExtractClearPrice(
      const fob::matching::v1::BatchResult& batch,
      const std::string& asset);

  static common::Decimal ComputeVolume24hFromBatch(
      const fob::matching::v1::BatchResult& batch,
      const std::string& asset);

  static std::vector<domain::DepthLevel> ExtractDepth(
      const domain::OrderBook& book,
      bool bid_side,
      uint32_t levels);
};

// Вычисляет EffectiveSpreadRecord из FillEvent + текущего mid.
// effectiveSpread = 2 * |execPrice - mid|
// effectiveSpreadBps = effectiveSpread / mid * 10000
class ComputeEffectiveSpread {
 public:
  static std::optional<domain::EffectiveSpreadRecord> FromFillEvent(
      const fob::matching::v1::FlowFill& fill,
      const std::string& fill_id,
      const std::string& batch_id,
      const common::Decimal& current_mid,
      domain::Timestamp ts);
};

}  // namespace cex::market_data::app
