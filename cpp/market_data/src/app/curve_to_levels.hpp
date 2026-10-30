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

#include <string>
#include <unordered_map>
#include <vector>

#include "fob/venue/v1/venue.pb.h"

#include "domain/external_order_level.hpp"
#include "domain/vectorize.hpp"  // VectorizeResult (ADR-052 two-sided)

namespace cex::market_data::app {

/// Разложить FOB-кривую в дискретные уровни (bid+ask). Пустые/невалидные шаги
/// пропускаются. Детерминировано (без random/таймеров) → пригодно для replay.
std::vector<domain::ExternalOrderLevel> LevelsFromCurve(
    const fob::venue::v1::VenueLiquidityCurve& curve, std::int32_t decimal_scale = 12);

/// ADR-051 (модель A): ОДИН линейный сегмент на сторону венью. Из кривой берётся
/// anchor-цена a=p_of_q[0] (top-of-book), глубина q_max=q_grid[last] и наклон через
/// d_hl_override=|p_of_q[last]−p_of_q[0]| (ценовой диапазон = |b|·q_max ⇒ D=|b|).
/// Даёт ≤2 уровня (bid+ask) вместо лестницы. Детерминировано (для replay).
std::vector<domain::ExternalOrderLevel> LinearSegmentsFromCurve(
    const fob::venue::v1::VenueLiquidityCurve& curve, std::int32_t decimal_scale = 12);

/// ADR-052 (academic двусторонний): набор кривых венью → общий AssetBasis +
/// ОДИН двусторонний сегмент на кривую (venue,pair). Сегмент: w = e_base − e_quote
/// (чистое направление), anchor a = mid, slope m = средний наклон сторон,
/// q_max = +Q_ask, q_min = −Q_bid (знаковый box). Требует обе стороны (mid).
/// Детерминировано (лексикографический basis, стабильный порядок кривых).
/// ADR-053: safe-translator наклон/anchor берутся из curve.safe_translator()
/// (посчитан venues ИЗ СЫРОГО стакана на стадии построения кривой). Клиринг
/// потребляет КРИВУЮ, не сырой стакан. Fallback (нет safe_translator) — из FOB.
domain::VectorizeResult TwoSidedSegmentsFromCurves(
    const std::vector<fob::venue::v1::VenueLiquidityCurve>& curves,
    std::int32_t decimal_scale = 12);

}  // namespace cex::market_data::app
