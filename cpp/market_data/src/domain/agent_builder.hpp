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
//   dead_zone c = [taker_fee_pm] + guard_mult·½·spread_pm
//                 + [k·σ·√age_sec]                          [‰]
//   (полка по умолчанию 0: half_spread_mult=0, комиссия не в полке; age-член —
//    ADR-075, k=0 по умолчанию ⇒ тоже 0. См. AgentBuilderConfig.)
//
// Чистая детерминированная логика (replay). α/c/mid — ‰-коэффициенты кривой
// (диагностика/параметры), не ledger-деньги → double допустим (.claude/rules/data.md).
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
  // ADR-075 (этап 4 F-18-remediation, причина 2): age-aware полка. Устаревший снимок
  // несёт неопределённость цены ⇒ добавляем к полке c += k·σ·√age (‰), чтобы клиринг не
  // гонял поток по «кажущемуся базису» от рассинхрона возрастов снимков (на dev ≈3.8‱
  // от разницы возрастов 20с). Деф k=0 ⇒ член нулевой (прежнее поведение). Поля
  // заполняет caller (market_data_uc): age снимка, σ актива (‰), k из env CE_AGE_DEAD_ZONE_K.
  double snapshot_age_ms = 0.0;   // возраст снимка (ms) на момент окна
  double sigma_pm = 0.0;          // σ актива в ‰ (из ce_asset_volatility)
  double age_dead_zone_k = 0.0;   // k; 0 ⇒ age-полка выключена
  // ADR-077 этап B: способ α_ext. 4 (деф) — прежний минорант min_i V_i/δ_i, поведение
  // не меняется. 5 — M5: регрессия предельного сдвига δ(V)=a+b·V по равномерной сетке
  // объёма до V_cal (Huber), α=1/b. Любой сбой M5 откатывается на M4 (причина в QuoteAgent).
  int alpha_method = 4;
  double depth_window_quote = 0.0;  // V_cal в единицах котируемой валюты (тег кривой); 0 ⇒ вся глубина стороны
  int m5_grid_points = 32;          // число узлов сетки объёма
  double m5_min_coverage = 0.5;     // сторона обязана покрывать эту долю V_cal, иначе откат на M4
  // При M5 полка ≥ m·½спред. По умолчанию 0 ⇒ полки нет (решение владельца 2026-10-15: пока без комиссии
  // и без полки); спред при этом остаётся только в свободном члене a регрессии (диагностика).
  double m5_half_spread_mult = 0.0;
};

struct QuoteAgent {
  std::string venue_id;
  std::string pair;
  double anchor_pm = 0.0;    // σ*, ‰
  double depth = 0.0;        // α (quote units на ‰)
  double dead_zone_pm = 0.0; // c, ‰
  // ADR-079 диагностика: из чего сложена полка (c = commission + spread + age), клиринг не читает.
  double dz_commission_pm = 0.0;
  double dz_spread_pm = 0.0;
  double dz_age_pm = 0.0;
  bool valid = false;
  std::string reason;        // причина, если !valid
  // ADR-077 диагностика (не влияет на клиринг): какой способ реально использован.
  int alpha_method_used = 4;     // 4 или 5
  double alpha_m4 = 0.0;         // α_ext по миноранту (всегда считается)
  double alpha_m5 = 0.0;         // α_ext по M5 (0 ⇒ не считался / откат)
  double m5_intercept_pm = 0.0;  // свободный член a регрессии (≈ ½спреда), ‰
  bool m5_negative_intercept = false;  // a<0: выпуклая книга (флаг, не откат)
  std::string m5_fallback;       // причина отката на M4 ("" ⇒ M5 применён или не запрашивался)
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

// ADR-077 этап B (M5). Результат подгонки одной стороны.
struct M5Side {
  bool ok = false;
  double alpha = 0.0;          // 1/b, quote units на ‰
  double intercept_pm = 0.0;   // a, ‰
  std::string why;             // причина отказа
};

inline double MedianAbs(std::vector<double> v) {
  for (double& x : v) x = std::fabs(x);
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  if (n == 0) return 0.0;
  return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Прямая y=a+b·x методом наименьших квадратов с весами Хьюбера (IRLS). Детерминирована:
// фиксированное число итераций, масштаб по MAD. false — вырожденный x.
inline bool HuberLine(const std::vector<double>& x, const std::vector<double>& y,
                      double* a_out, double* b_out) {
  const std::size_t n = x.size();
  if (n < 3 || y.size() != n) return false;
  std::vector<double> w(n, 1.0);
  double a = 0.0, b = 0.0;
  for (int it = 0; it < 30; ++it) {
    double sw = 0.0, sx = 0.0, sy = 0.0;
    for (std::size_t i = 0; i < n; ++i) { sw += w[i]; sx += w[i] * x[i]; sy += w[i] * y[i]; }
    const double mx = sx / sw, my = sy / sw;
    double sxx = 0.0, sxy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      sxx += w[i] * (x[i] - mx) * (x[i] - mx);
      sxy += w[i] * (x[i] - mx) * (y[i] - my);
    }
    if (!(sxx > 0.0)) return false;
    b = sxy / sxx;
    a = my - b * mx;
    std::vector<double> r(n);
    for (std::size_t i = 0; i < n; ++i) r[i] = y[i] - (a + b * x[i]);
    const double s = 1.4826 * MedianAbs(r);
    if (!(s > 1e-12)) break;  // идеальная подгонка
    const double c = 1.345 * s;
    double change = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double nw = std::fabs(r[i]) <= c ? 1.0 : c / std::fabs(r[i]);
      change = std::fmax(change, std::fabs(nw - w[i]));
      w[i] = nw;
    }
    if (change < 1e-9) break;
  }
  *a_out = a;
  *b_out = b;
  return std::isfinite(a) && std::isfinite(b);
}

// M5 для одной стороны: предельный сдвиг уровня δ(‰) против накопленной стоимости V на
// равномерной сетке (0, V_max]; V_max = min(глубина стороны, V_cal). α = V_max/b', где
// b' — наклон по нормированному V/V_max.
inline M5Side FitM5Side(std::vector<std::pair<double, double>> px_sz, double mid,
                        const AgentBuilderConfig& cfg) {
  M5Side out;
  std::sort(px_sz.begin(), px_sz.end(), [mid](const auto& p, const auto& q) {
    return std::fabs(p.first - mid) < std::fabs(q.first - mid);
  });
  std::vector<double> cum, shift;
  double v = 0.0;
  for (const auto& [price, size] : px_sz) {
    if (price <= 0.0 || size <= 0.0) continue;
    v += price * size;
    cum.push_back(v);
    shift.push_back(std::fabs(1000.0 * std::log(price / mid)));
  }
  if (cum.size() < 3) { out.why = "few_levels"; return out; }
  const double total = cum.back();
  double vmax = total;
  if (cfg.depth_window_quote > 0.0) {
    if (total < cfg.m5_min_coverage * cfg.depth_window_quote) { out.why = "coverage"; return out; }
    vmax = std::fmin(total, cfg.depth_window_quote);
  }
  const int m = std::max(8, std::min(256, cfg.m5_grid_points));
  std::vector<double> gx, gy;
  gx.reserve(static_cast<std::size_t>(m));
  gy.reserve(static_cast<std::size_t>(m));
  std::size_t i = 0;
  for (int j = 1; j <= m; ++j) {
    const double x = static_cast<double>(j) / static_cast<double>(m);
    const double target = vmax * x;
    while (i + 1 < cum.size() && cum[i] < target) ++i;  // уровень, в котором лежит объём target
    gx.push_back(x);
    gy.push_back(shift[i]);
  }
  std::size_t distinct = 1;
  for (std::size_t k = 1; k < gy.size(); ++k) {
    if (std::fabs(gy[k] - gy[k - 1]) > 1e-12) ++distinct;
  }
  if (distinct < 3) { out.why = "few_distinct"; return out; }
  double a = 0.0, b = 0.0;
  if (!HuberLine(gx, gy, &a, &b)) { out.why = "degenerate"; return out; }
  if (!(b > 0.0)) { out.why = "slope"; return out; }
  out.alpha = vmax / b;
  out.intercept_pm = a;
  out.ok = std::isfinite(out.alpha) && out.alpha > 0.0;
  if (!out.ok) out.why = "alpha";
  return out;
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
  double alpha_ext = std::fmin(aext_bid, aext_ask);
  a.alpha_m4 = alpha_ext;
  if (cfg.alpha_method == 5) {
    // ADR-077 этап B: α по регрессии на окне объёма; min по сторонам (как у M4).
    const detail::M5Side fb = detail::FitM5Side(bids, mid, cfg);
    const detail::M5Side fa = detail::FitM5Side(asks, mid, cfg);
    if (fb.ok && fa.ok) {
      alpha_ext = std::fmin(fb.alpha, fa.alpha);
      a.alpha_method_used = 5;
      a.alpha_m5 = alpha_ext;
      a.m5_intercept_pm = 0.5 * (fb.intercept_pm + fa.intercept_pm);
      a.m5_negative_intercept = fb.intercept_pm < 0.0 || fa.intercept_pm < 0.0;
    } else {
      a.m5_fallback = !fb.ok ? ("bid:" + fb.why) : ("ask:" + fa.why);  // остаёмся на M4
    }
  }
  a.depth = cfg.theta * alpha_ext;

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
  double guard_mult =
      (cfg.wide_spread_guard_pm > 0.0 && half_spread_pm >= cfg.wide_spread_guard_pm)
          ? 1.0
          : cfg.half_spread_mult;
  // ADR-077: при M5 спред ушёл в свободный член регрессии и из наклона пропал — полка обязана
  // его содержать, иначе спред исчезает и из наклона, и из полки.
  if (a.alpha_method_used == 5) guard_mult = std::fmax(guard_mult, cfg.m5_half_spread_mult);
  // ADR-065 Q1: комиссия входит в полку ТОЛЬКО при commission_in_polka (§A1-совместимость);
  // по умолчанию (§6.4) полка = множитель·½спред, комиссия — только в порогах band (ledger).
  // ADR-075: age-aware член полки c += k·σ·√(age_sec), ‰ (деф k=0 ⇒ 0, прежнее поведение).
  const double age_floor_pm =
      (cfg.age_dead_zone_k > 0.0 && cfg.sigma_pm > 0.0 && cfg.snapshot_age_ms > 0.0)
          ? cfg.age_dead_zone_k * cfg.sigma_pm * std::sqrt(cfg.snapshot_age_ms / 1000.0)
          : 0.0;
  a.dz_commission_pm = cfg.commission_in_polka ? taker_fee_pm : 0.0;
  a.dz_spread_pm = guard_mult * half_spread_pm;
  a.dz_age_pm = age_floor_pm;
  a.dead_zone_pm = a.dz_commission_pm + a.dz_spread_pm + a.dz_age_pm;

  a.valid = a.depth > 0.0;
  if (!a.valid) a.reason = "non-positive depth";
  return a;
}

}  // namespace cex::market_data::domain
