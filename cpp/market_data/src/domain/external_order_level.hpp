#pragma once
// ============================================================================
// external_order_level.hpp — F-05A (T-F05A-201). market_data domain VO.
//
// Один дискретный уровень внешнего ордербука (venue), нормализованный из F-11
// (venue.liquidity.fob / venue.snapshots). Вход векторизации: набор таких уровней
// → столбцы матрицы W (см. vectorize.hpp). Деньги/qty — Decimal (§9).
// ============================================================================

#include <cstdint>
#include <string>

#include "cex/common/decimal.hpp"

namespace cex::market_data::domain {

enum class LevelSide { kBid, kAsk };

inline const char* ToString(LevelSide s) {
  return s == LevelSide::kBid ? "bid" : "ask";
}

struct ExternalOrderLevel {
  std::string venue_id;
  std::string source_order_id;   ///< id уровня/ордера на venue (трассировка → w_i)
  std::string pair;              ///< "BTC/USDT"
  std::string base_asset;        ///< X
  std::string quote_asset;       ///< Y
  LevelSide side{LevelSide::kBid};

  cex::common::Decimal price{};              ///< котируемая цена P (quote за base)
  cex::common::Decimal quantity{};           ///< объём в base units
  cex::common::Decimal remaining_quantity{}; ///< остаток base units (если задан)
  cex::common::Decimal effective_price{};    ///< P_eff, если посчитан upstream; 0 ⇒ derive

  // Буферы для P_eff (в bps). Не деньги — коэффициенты корректировки цены.
  double fees_bps{0.0};
  double latency_buffer_bps{0.0};
  double slippage_buffer_bps{0.0};

  std::int64_t ts_event_ms{0};   ///< время события (для staleness в UC, T-F05A-205)
};

}  // namespace cex::market_data::domain
