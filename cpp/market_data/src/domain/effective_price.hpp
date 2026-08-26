#pragma once
// ============================================================================
// effective_price.hpp — F-05A (T-F05A-203). market_data domain. Header-only.
//
// P_eff — цена, ухудшенная против тейкера на fees/latency/slippage (в bps).
// Bid (продаём в чужой bid): P_eff = P·(1 − f); Ask (берём из чужого ask):
// P_eff = P·(1 + f), где f = (fees+latency+slippage)/1e4.
//
// Возвращается double: P_eff — ВХОД СОЛВЕРА (коэффициент w[Y] = ∓P_eff), а не
// ledger-сумма (§9: double допустим, т.к. результат не попадает в проводки —
// проводки идут по клиринговой цене π и x downstream). Если upstream уже задал
// level.effective_price (units != 0) — используем его.
// ============================================================================

#include "cex/common/decimal.hpp"
#include "domain/external_order_level.hpp"

namespace cex::market_data::domain {

inline double EffectivePriceDouble(const ExternalOrderLevel& lvl) {
  if (lvl.effective_price.units != 0) {
    return static_cast<double>(lvl.effective_price);
  }
  const double p = static_cast<double>(lvl.price);
  const double f =
      (lvl.fees_bps + lvl.latency_buffer_bps + lvl.slippage_buffer_bps) / 10000.0;
  return lvl.side == LevelSide::kBid ? p * (1.0 - f) : p * (1.0 + f);
}

}  // namespace cex::market_data::domain
