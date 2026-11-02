#pragma once
// ADR-082, шаг 1: множитель глубины котировки агента по коэффициенту исполнения биржи f.
// Чистая функция без ввода-вывода. Глубина агента — метрика кривой (double), как и прежняя θ·α_ext; в проводки не попадает.

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace cex::market_data::domain {

// Измерение биржи по паре, как его публикует venues (venue_liquidity_stats): f и число заявок в f с забыванием.
struct VenueFillStat {
  double fill_coef{1.0};
  double fill_orders_eff{0.0};
};
// Ключ снимка: venue + "|" + канонический ключ пары (BASE+QUOTE без разделителей, верхний регистр: BTCUSDT, ETHBTC).
using VenueFillSnapshot = std::map<std::string, VenueFillStat>;
inline std::string VenueFillKey(const std::string& venue, const std::string& symbol) { return venue + "|" + symbol; }

struct FillDepthScaleConfig {
  double f_min{0.1};    // нижняя граница множителя: глубина не обнуляется, и тонкая биржа остаётся в клиринге
  double n0{5.0};       // псевдо-заявки с f = 1: сколько заявок нужно, чтобы доверять измеренному f наполовину
  // Нижняя граница опорного f при нормировке на лучшую биржу актива: множитель = f_eff / max(лучший f_eff, ref_min).
  // 1 ⇒ нормировки нет (чистый абсолютный f, как в формуле эксперта); малое значение ⇒ чисто относительное сравнение бирж.
  // Защита от случая, когда плохи все биржи сразу: лучшая из них тоже получит множитель меньше 1 (f_eff / ref_min).
  double ref_min{0.5};
  // В опорный максимум входят только биржи, у которых число заявок с забыванием не ниже этого порога: оценка по нескольким давним
  // заявкам после сжатия к единице близка к 1 и закрепила бы опору на 1, превратив нормировку в абсолютный f.
  double ref_min_orders{5.0};
};

// f — измеренная доля исполнения пары (0…1), n_eff — число заявок в f с забыванием.
// Сжатие к единице: f_eff = (n_eff·f + n0) / (n_eff + n0). Нет заявок (n_eff = 0) ⇒ 1; много заявок ⇒ измеренный f; когда
// заявки перестают приходить, n_eff затухает и значение возвращается к 1 — оценка, застывшая на нескольких давних заявках,
// не режет глубину навсегда. Результат в [0, 1]. Нечисловой вход ⇒ 1 (ничего не режем).
inline double ShrunkFill(double f, double n_eff, double n0_cfg) {
  if (!std::isfinite(f) || !std::isfinite(n_eff) || n_eff <= 0.0) return 1.0;
  const double n0 = std::isfinite(n0_cfg) ? std::max(0.0, n0_cfg) : 0.0;
  const double fc = std::min(1.0, std::max(0.0, f));
  return std::min(1.0, std::max(0.0, (n_eff * fc + n0) / (n_eff + n0)));
}

// Абсолютный множитель: f_eff, обрезанный снизу f_min. Результат в [f_min, 1].
inline double FillDepthScale(double f, double n_eff, const FillDepthScaleConfig& cfg) {
  const double lo = std::isfinite(cfg.f_min) ? std::min(1.0, std::max(0.0, cfg.f_min)) : 0.0;
  return std::min(1.0, std::max(lo, ShrunkFill(f, n_eff, cfg.n0)));
}

// Множитель глубины котировки (venue, base/quote) по снимку с нормировкой на лучшую биржу того же актива:
//   s = clamp( f_eff(venue) / max( max_{биржи с данными по этой паре} f_eff, ref_min ), f_min, 1 ).
// Лучшая по паре биржа получает 1 (если её f_eff не ниже ref_min), остальные пропорционально отставанию. Общий шум и всплески
// заявок, роняющие f у всех бирж сразу, не режут всех; режется отставание от лучшей. Пары нет в снимке (нет заявок, запись
// устарела, источник выключен) ⇒ 1; биржи без данных или с числом заявок ниже ref_min_orders в опорный максимум не входят.
// Ключ строится как у venues: base + quote.
inline double ResolveFillDepthScale(const VenueFillSnapshot& snapshot, const std::string& venue, const std::string& base,
                                    const std::string& quote, const FillDepthScaleConfig& cfg) {
  const std::string symbol = base + quote;
  const auto own = snapshot.find(VenueFillKey(venue, symbol));
  if (own == snapshot.end()) return 1.0;
  const double g = ShrunkFill(own->second.fill_coef, own->second.fill_orders_eff, cfg.n0);
  double ref = 0.0;
  for (const auto& [key, st] : snapshot) {
    const auto bar = key.rfind('|');
    if (bar == std::string::npos || key.compare(bar + 1, std::string::npos, symbol) != 0) continue;
    if (!(st.fill_orders_eff >= cfg.ref_min_orders)) continue;  // оценка без достаточного числа заявок опорой не служит
    ref = std::max(ref, ShrunkFill(st.fill_coef, st.fill_orders_eff, cfg.n0));
  }
  const double ref_min = std::isfinite(cfg.ref_min) ? std::min(1.0, std::max(0.0, cfg.ref_min)) : 0.0;
  ref = std::max(ref, ref_min);
  if (!(ref > 1e-9)) return 1.0;  // все f нулевые и опорной границы нет: сравнивать не с чем, ничего не режем
  const double lo = std::isfinite(cfg.f_min) ? std::min(1.0, std::max(0.0, cfg.f_min)) : 0.0;
  return std::min(1.0, std::max(lo, g / ref));
}

}  // namespace cex::market_data::domain
