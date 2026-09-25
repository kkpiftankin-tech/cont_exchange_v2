#pragma once
// ============================================================================
// agent_builder.hpp — F-05A CE (T-CEA-104, ADR-055 §A1). market_data domain.
//
// Построение book-derived параметров агента-ПЕРЕВОДЧИКА (плечо котировки) из
// дискретных уровней внешней книги ОДНОЙ (venue, pair):
//   anchor σ*  = mid_pm = 1000·ln(mid / P0)                 [‰]  (mid = (bid₀+ask₀)/2)
//   depth  α   = θ · min( α_ext(bid), α_ext(ask) )          [quote units на ‰]
//              α_ext = min_i ( V_i / δ_i ),  V_i = Σ_{k≤i} price_k·size_k,
//              δ_i = |1000·ln(price_i / mid)|               [консервативный минорант]
//   dead_zone c = taker_fee_pm + ½·spread_pm                [‰]
//
// Чистая детерминированная логика (replay). α/c/mid — ‰-коэффициенты кривой
// (диагностика/параметры), не ledger-деньги → double допустим (CLAUDE.md §9).
// Плечи ПЕРЕВОДА и ЗАПАСА строятся не из книги, а из капитала/риска (ADR-055) —
// здесь не считаются. Эталон Python валидирует клиринг при заданных α/c; сам
// вывод α/c из книги — по формуле §A1 (golden нет).
// ============================================================================

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "domain/external_order_level.hpp"

namespace cex::market_data::domain {

struct AgentBuilderConfig {
  double theta = 0.5;            // haircut глубины (0.3..0.7)
  double reference_price = 0.0;  // P0 для mid_pm; ≤0 ⇒ anchor=0 (вырожденный такт)
  // Настраиваемая с фронта комиссия тейкера, bps. По умолчанию 0 ⇒ мёртвая зона
  // c = ½·spread (при тесном спреде ≈ линейная кривая без полки-комиссии; решение
  // владельца 2026-09-16). <0 ⇒ брать комиссию из стакана venue (прежнее поведение,
  // выставляется с фронта). Runtime из f05a_clearing_config.ce_taker_fee_bps.
  double taker_fee_bps_override = 0.0;
  // Множитель члена ½·spread в полке (dead_zone). По умолчанию 0 => ПОЛКА НУЛЕВАЯ
  // (переводчик клирит ликвидность книг до мида, а не только при разрыве > ½спреда;
  // решение владельца 2026-09-16 — «полку по умолчанию сделать нулевой»). >0 =>
  // вернуть долю полуспреда как барьер маркет-мейкера (выставляется с фронта/конфига).
  double half_spread_mult = 0.0;
  // Порог «широкого спреда» (‰): книги с half_spread ≥ порога считаются тонкими/
  // ненадёжными (почти пустой стакан, кросс-пара с рассинхроном) и СОХРАНЯЮТ полный
  // ½·спред как полку — иначе клиринг гоняет их шум (напр. якорь ETH/BTC ~97‰) через
  // ликвидные рёбра, раздувая позиции. Ликвидные книги (спред ~1‰) < порога ⇒ полка
  // по half_spread_mult (0 по умолчанию). Дефолт 20‰ отсекает кросс-пару (~144‰),
  // но пускает CEX (~1‰) и uniswap (~6‰). 0 ⇒ порог выключен (защиты нет).
  double wide_spread_guard_pm = 20.0;
  // ADR-065 Q1 (решение владельца §6.4): комиссия НЕ в полке кривой, а ТОЛЬКО в
  // порогах band (ledger). false (дефолт) ⇒ полка = ½·spread без комиссии. true ⇒
  // §A1-поведение (полка = комиссия + ½·spread) — обратимо через env CE_COMMISSION_IN_POLKA.
  bool commission_in_polka = false;
};

struct QuoteAgent {
  std::string venue_id;
  std::string pair;
  double anchor_pm = 0.0;    // σ*, ‰
  double depth = 0.0;        // α (quote units на ‰)
  double dead_zone_pm = 0.0; // c, ‰
  bool valid = false;
  std::string reason;        // причина, если !valid
};

namespace detail {

// α_ext стороны: min_i (V_i/δ_i) по уровням, отсортированным по |цена−mid|.
// V_i — накопленная стоимость (price·size), δ_i — расстояние до mid в ‰.
inline double AlphaExtSide(std::vector<std::pair<double, double>> px_sz, double mid) {
  // сортируем по расстоянию от mid (ближние уровни первыми)
  std::sort(px_sz.begin(), px_sz.end(),
            [mid](const auto& a, const auto& b) {
              return std::fabs(a.first - mid) < std::fabs(b.first - mid);
            });
  double cum_v = 0.0, best = -1.0;
  for (const auto& [price, size] : px_sz) {
    if (price <= 0.0 || size <= 0.0) continue;
    cum_v += price * size;
    const double delta = std::fabs(1000.0 * std::log(price / mid));  // ‰
    if (delta < 1e-9) continue;  // уровень на mid — не ограничивает минорант
    const double ratio = cum_v / delta;
    if (best < 0.0 || ratio < best) best = ratio;
  }
  return best;  // <0 ⇒ нет пригодных уровней
}

}  // namespace detail

// Построить book-derived quote-агента из уровней одной (venue, pair).
inline QuoteAgent BuildQuoteAgent(const std::vector<ExternalOrderLevel>& levels,
                                  const AgentBuilderConfig& cfg) {
  QuoteAgent a;
  if (!levels.empty()) { a.venue_id = levels.front().venue_id; a.pair = levels.front().pair; }

  double best_bid = -1.0, best_ask = -1.0;
  double taker_fee_bps = 0.0;
  std::vector<std::pair<double, double>> bids, asks;
  for (const auto& lv : levels) {
    const double price = static_cast<double>(lv.price);
    const double qty = static_cast<double>(lv.quantity);
    if (price <= 0.0 || qty <= 0.0) continue;
    taker_fee_bps = std::fmax(taker_fee_bps, lv.fees_bps);
    if (lv.side == LevelSide::kBid) {
      bids.emplace_back(price, qty);
      best_bid = std::fmax(best_bid, price);
    } else {
      asks.emplace_back(price, qty);
      best_ask = (best_ask < 0.0) ? price : std::fmin(best_ask, price);
    }
  }
  if (best_bid <= 0.0 || best_ask <= 0.0) { a.reason = "no two-sided book"; return a; }

  const double mid = 0.5 * (best_bid + best_ask);
  a.anchor_pm = (cfg.reference_price > 0.0) ? 1000.0 * std::log(mid / cfg.reference_price) : 0.0;

  const double aext_bid = detail::AlphaExtSide(bids, mid);
  const double aext_ask = detail::AlphaExtSide(asks, mid);
  if (aext_bid < 0.0 || aext_ask < 0.0) { a.reason = "empty side for alpha_ext"; return a; }
  a.depth = cfg.theta * std::fmin(aext_bid, aext_ask);

  const double half_spread_pm = 0.5 * std::fabs(1000.0 * std::log(best_ask / best_bid));
  // Настраиваемая с фронта комиссия: override≥0 замещает комиссию стакана (0 ⇒ линейная
  // кривая без полки-комиссии, остаётся только ½·spread).
  const double eff_fee_bps =
      cfg.taker_fee_bps_override >= 0.0 ? cfg.taker_fee_bps_override : taker_fee_bps;
  const double taker_fee_pm = eff_fee_bps / 10.0;  // bps→‰ (1‰ = 10 bps)
  // Полка = комиссия + множитель·½спред. Ликвидная книга (спред < порога) с
  // множителем 0 ⇒ ПОЛКА НУЛЕВАЯ: клиринг двигает поток на любой девиации,
  // переводчики клирят ликвидность. Тонкая/ненадёжная книга (спред ≥ порога) ⇒
  // полный ½спред как барьер, иначе её шум (напр. ETH/BTC ~97‰) гоняется через
  // ликвидные рёбра и раздувает позиции. Порог 0 ⇒ выключен.
  const double guard_mult =
      (cfg.wide_spread_guard_pm > 0.0 && half_spread_pm >= cfg.wide_spread_guard_pm)
          ? 1.0
          : cfg.half_spread_mult;
  // ADR-065 Q1: комиссия входит в полку ТОЛЬКО при commission_in_polka (§A1-совместимость);
  // по умолчанию (§6.4) полка = множитель·½спред, комиссия — только в порогах band (ledger).
  a.dead_zone_pm = (cfg.commission_in_polka ? taker_fee_pm : 0.0) + guard_mult * half_spread_pm;

  a.valid = a.depth > 0.0;
  if (!a.valid) a.reason = "non-positive depth";
  return a;
}

}  // namespace cex::market_data::domain
