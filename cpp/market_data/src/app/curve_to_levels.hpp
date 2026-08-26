#pragma once
// ============================================================================
// curve_to_levels.hpp — F-05A (T-F05A-205). market_data app mapper.
//
// Реконструкция дискретных внешних уровней из непрерывной F-11 FOB-кривой
// (VenueLiquidityCurve, вход D2 = venue.liquidity.fob). Каждый шаг сетки
// [q_grid[k-1], q_grid[k]] трактуется как «уровень» с marginal-ценой p_of_q[k] и
// объёмом (q_grid[k] − q_grid[k-1]). bid_curve → bid-уровни, ask_curve → ask.
// Далее уровни идут в domain::Vectorize (w_i, сегменты).
//
// proto→domain (транспорт знает proto). Цены/объёмы кривой (double) квантуются в
// Decimal на границе (venue-ingest); P_eff/буферы — в vectorize.
// ============================================================================

#include <vector>

#include "fob/venue/v1/venue.pb.h"

#include "domain/external_order_level.hpp"

namespace cex::market_data::app {

/// Разложить FOB-кривую в дискретные уровни (bid+ask). Пустые/невалидные шаги
/// пропускаются. Детерминировано (без random/таймеров) → пригодно для replay.
std::vector<domain::ExternalOrderLevel> LevelsFromCurve(
    const fob::venue::v1::VenueLiquidityCurve& curve, std::int32_t decimal_scale = 12);

}  // namespace cex::market_data::app
