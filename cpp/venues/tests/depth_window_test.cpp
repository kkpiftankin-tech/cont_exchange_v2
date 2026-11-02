#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "domain/depth_window.hpp"

using cex::venues::domain::ComputeDepthWindow;
using cex::venues::domain::DepthLevel;
using cex::venues::domain::DepthWindowConfig;
using cex::venues::domain::DepthWindowInputs;
using cex::venues::domain::DepthWindowResult;
using cex::venues::domain::DepthWindowState;
using cex::venues::domain::LevelsToReachVolume;
using cex::venues::domain::NextRequestLimit;
using cex::venues::domain::QBarFromInvariance;
using cex::venues::domain::SlippageBpsAtVolume;

namespace {

bool Check(bool cond, const std::string& msg) {
  if (!cond) std::cerr << "[FAIL] " << msg << std::endl;
  return cond;
}

bool Near(double a, double b, double rel) {
  return std::fabs(a - b) <= rel * std::max(1.0, std::fabs(b));
}

// Игрушечная сторона покупок: стоимость уровней 1000, 1980, 2940 → накоплено 1000, 2980, 5920.
std::vector<DepthLevel> ToyBids() { return {{100.0, 10.0}, {99.0, 20.0}, {98.0, 30.0}}; }
std::vector<DepthLevel> ToyAsks() { return {{101.0, 10.0}, {102.0, 20.0}, {103.0, 30.0}}; }

// Глубокая симметричная книга: уровень i (1..N) стоит 1000 долл. и лежит на 0,5 б.п. глубже предыдущего.
std::vector<DepthLevel> DeepSide(bool bid, std::size_t n, double mid = 100.0) {
  std::vector<DepthLevel> v;
  for (std::size_t i = 1; i <= n; ++i) {
    const double p = bid ? mid * (1.0 - 0.00005 * static_cast<double>(i))
                         : mid * (1.0 + 0.00005 * static_cast<double>(i));
    v.push_back({p, 1000.0 / p});
  }
  return v;
}

bool TestQBarInvariance() {
  DepthWindowConfig cfg;
  double mu = 0.0;
  const double q = QBarFromInvariance(1.9e9, 0.011, cfg, &mu);
  // W = 0.011·1.9e9 = 2.09e7; (W/8e5)^(2/3) ≈ 8.80; μ ≈ 748; q̄ ≈ 1.27e6.
  bool ok = Check(Near(mu, 748.0, 0.01), "μ ≈ 748 для Binance BTC/USDT (замер 2026-10-15)");
  ok = Check(Near(q, 1.27e6, 0.02), "q̄ ≈ 1,27 млн USD") && ok;
  // C-1 документа: q̄·μ·ζ = V$ (тождество построения).
  ok = Check(Near(q * mu * cfg.zeta, 1.9e9, 1e-12), "C-1: q̄·μ·ζ = V$") && ok;
  // Эталонная точка W = W*: μ = μ*, q̄ = V$/(ζ·μ*).
  const double v = 8.0e5 / 0.01;
  double mu_ref = 0.0;
  const double q_ref = QBarFromInvariance(v, 0.01, cfg, &mu_ref);
  ok = Check(Near(mu_ref, 85.0, 1e-12) && Near(q_ref, v / (2.0 * 85.0), 1e-12),
             "при W = W* получаем μ = μ*") && ok;
  ok = Check(QBarFromInvariance(0.0, 0.01, cfg) == 0.0, "нет объёма ⇒ 0") && ok;
  ok = Check(QBarFromInvariance(1e9, 0.0, cfg) == 0.0, "нет волатильности ⇒ 0") && ok;
  ok = Check(QBarFromInvariance(1e9, std::nan(""), cfg) == 0.0, "NaN σ ⇒ 0") && ok;
  return ok;
}

bool TestLevelsToReach() {
  bool reached = false;
  const auto b = ToyBids();
  bool ok = Check(LevelsToReachVolume(b, 4000.0, &reached) == 3 && reached, "4000 ⇒ 3 уровня");
  ok = Check(LevelsToReachVolume(b, 1000.0, &reached) == 1 && reached, "ровно D1 ⇒ 1 уровень") && ok;
  ok = Check(LevelsToReachVolume(b, 1000.0001, &reached) == 2 && reached, "чуть больше D1 ⇒ 2") && ok;
  ok = Check(LevelsToReachVolume(b, 10000.0, &reached) == 3 && !reached,
             "не хватает стакана ⇒ все 3, reached=false (без экстраполяции)") && ok;
  std::vector<DepthLevel> dirty = {{100.0, 10.0}, {std::nan(""), 5.0}, {99.0, 0.0}, {98.0, 30.0}};
  ok = Check(LevelsToReachVolume(dirty, 2000.0, &reached) == 4 && reached,
             "мусорные уровни пропускаются, но позиции сохраняются (для обрезки снимка)") && ok;
  return ok;
}

bool TestSlippagePartialLevel() {
  const auto b = ToyBids();
  double d = 0.0;
  const bool ok_call = SlippageBpsAtVolume(b, 100.5, 4000.0, &d);
  // Количество: 10 + 20 + (4000−2980)/98; VWAP = 4000/qty.
  const double qty = 10.0 + 20.0 + (4000.0 - 2980.0) / 98.0;
  const double expect = 1e4 * std::fabs(std::log((4000.0 / qty) / 100.5));
  bool ok = Check(ok_call, "сторона покрывает объём");
  ok = Check(Near(d, expect, 1e-12), "δ с частичным последним уровнем") && ok;
  ok = Check(!SlippageBpsAtVolume(b, 100.5, 10000.0, &d), "стороны не хватает ⇒ false") && ok;
  ok = Check(!SlippageBpsAtVolume(b, 0.0, 4000.0, &d), "mid=0 ⇒ false") && ok;
  return ok;
}

bool TestWindowToyBook() {
  DepthWindowConfig cfg;
  DepthWindowInputs in;
  in.q_bar_override_usd = 400.0;  // V_cal = 10·400 = 4000
  DepthWindowState st;
  const DepthWindowResult r = ComputeDepthWindow(cfg, in, ToyBids(), ToyAsks(), 100.5, &st);
  // Покупки: D = 1000, 2980, 5920 ⇒ 3. Продажи: 1010, 3050, 6140 ⇒ 3 (3050 < 4000 ≤ 6140).
  bool ok = Check(r.applied && !r.no_inputs, "окно применено");
  ok = Check(r.n_bid == 3 && r.n_ask == 3 && r.n == 3, "n = 3 по обеим сторонам") && ok;
  ok = Check(!r.truncated && !r.api_capped, "стакана хватает") && ok;
  ok = Check(r.coarse_tick, "n=3 < 5 ⇒ coarse_tick (низкая уверенность)") && ok;
  ok = Check(Near(r.v_cal_usd, 4000.0, 1e-12) && Near(r.q_bar_usd, 400.0, 1e-12), "V_cal = k·q̄") && ok;
  ok = Check(r.delta_n_bps > 0.0 && Near(r.alpha_chord, 4000.0 / r.delta_n_bps, 1e-12),
             "α_chord = V_cal/δ_n") && ok;
  ok = Check(st.has_prev && st.n_prev == 3 && Near(st.v_cal_ewma, 4000.0, 1e-12), "состояние сохранено") && ok;
  return ok;
}

bool TestTruncatedNoExtrapolation() {
  DepthWindowConfig cfg;
  DepthWindowInputs in;
  in.q_bar_override_usd = 1000.0;  // V_cal = 10 000 > глубины игрушечной книги (5920 / 6140)
  DepthWindowState st;
  const DepthWindowResult r = ComputeDepthWindow(cfg, in, ToyBids(), ToyAsks(), 100.5, &st);
  bool ok = Check(r.applied && r.truncated, "стакан тоньше окна ⇒ truncated");
  ok = Check(r.n == 3, "берём весь доступный стакан, не больше") && ok;
  ok = Check(!r.coarse_tick, "при truncated coarse_tick не ставится (причина — тонкий стакан)") && ok;
  ok = Check(r.delta_n_bps == 0.0 && r.alpha_chord == 0.0,
             "δ_n и α_chord не считаются без набранного объёма") && ok;
  return ok;
}

bool TestApiCap() {
  DepthWindowConfig cfg;
  cfg.max_levels = 50;
  DepthWindowInputs in;
  in.q_bar_override_usd = 100000.0;  // V_cal = 1 000 000 — нужно 1000 уровней по 1000 долл.
  DepthWindowState st;
  const auto bids = DeepSide(true, 50);
  const auto asks = DeepSide(false, 50);
  const DepthWindowResult r = ComputeDepthWindow(cfg, in, bids, asks, 100.0, &st);
  bool ok = Check(r.truncated && r.api_capped, "книга на потолке и объём не набран ⇒ api_capped");
  ok = Check(r.n == 50, "n упирается в потолок") && ok;
  // Стакан длиннее потолка, объём набирается только за потолком.
  cfg.max_levels = 20;
  DepthWindowState st2;
  in.q_bar_override_usd = 3000.0;  // V_cal = 30 000 ⇒ 30 уровней
  const DepthWindowResult r2 = ComputeDepthWindow(cfg, in, DeepSide(true, 100), DeepSide(false, 100), 100.0, &st2);
  ok = Check(r2.api_capped && r2.n == 20 && !r2.truncated, "n обрезан потолком, стакан глубже") && ok;
  return ok;
}

bool TestNoInputs() {
  DepthWindowConfig cfg;
  DepthWindowInputs in;  // ни объёма, ни σ, ни переопределения
  DepthWindowState st;
  const DepthWindowResult r = ComputeDepthWindow(cfg, in, ToyBids(), ToyAsks(), 100.5, &st);
  bool ok = Check(r.no_inputs && !r.applied, "нет входов ⇒ no_inputs (используется прежнее depth_levels)");
  ok = Check(!st.has_prev, "состояние не тронуто") && ok;
  in.q_bar_override_usd = 400.0;
  const DepthWindowResult r2 = ComputeDepthWindow(cfg, in, {}, ToyAsks(), 100.5, &st);
  ok = Check(r2.no_inputs, "пустая сторона ⇒ no_inputs") && ok;
  const DepthWindowResult r3 = ComputeDepthWindow(cfg, in, ToyBids(), ToyAsks(), 0.0, &st);
  ok = Check(r3.no_inputs, "mid=0 ⇒ no_inputs") && ok;
  return ok;
}

bool TestEwma() {
  DepthWindowConfig cfg;  // α = 0.2
  DepthWindowInputs in;
  DepthWindowState st;
  const auto bids = DeepSide(true, 400);
  const auto asks = DeepSide(false, 400);
  in.q_bar_override_usd = 1000.0;  // V_cal_raw = 10 000
  (void)ComputeDepthWindow(cfg, in, bids, asks, 100.0, &st);
  in.q_bar_override_usd = 2000.0;  // raw = 20 000, сглаженное = 0.2·20 000 + 0.8·10 000 = 12 000
  const DepthWindowResult r = ComputeDepthWindow(cfg, in, bids, asks, 100.0, &st);
  bool ok = Check(Near(r.v_cal_raw_usd, 20000.0, 1e-12), "сырое V_cal");
  ok = Check(Near(r.v_cal_usd, 12000.0, 1e-12), "EWMA: 0,2·20000 + 0,8·10000 = 12000") && ok;
  ok = Check(Near(st.v_cal_ewma, 12000.0, 1e-12), "EWMA в состоянии") && ok;
  return ok;
}

bool TestHysteresis() {
  DepthWindowConfig cfg;
  cfg.ewma_alpha = 1.0;  // без сглаживания — проверяем только гистерезис
  const auto bids = DeepSide(true, 400);
  const auto asks = DeepSide(false, 400);
  DepthWindowInputs in;
  auto run = [&](double v_cal, DepthWindowState* st) {
    in.q_bar_override_usd = v_cal / cfg.k;
    return ComputeDepthWindow(cfg, in, bids, asks, 100.0, st);
  };
  DepthWindowState st;
  bool ok = Check(run(100000.0, &st).n == 100, "старт: 100 уровней по 1000 долл.");
  ok = Check(run(110000.0, &st).n == 100, "110 внутри ±15 % от 100 ⇒ остаётся 100") && ok;
  ok = Check(run(90000.0, &st).n == 100, "90 внутри ±15 % ⇒ остаётся 100") && ok;
  ok = Check(run(120000.0, &st).n == 120, "120 вне +15 % ⇒ 120") && ok;
  ok = Check(run(100000.0, &st).n == 100, "100 вне −15 % от 120 (нижняя граница 102) ⇒ 100") && ok;
  return ok;
}

bool TestRequestLimit() {
  const std::vector<std::size_t> binance = {5, 10, 20, 50, 100, 500, 1000, 5000};
  bool ok = Check(NextRequestLimit(0, 1000, 1.25, binance) == 1000, "первый запрос — на потолке API");
  ok = Check(NextRequestLimit(20, 1000, 1.25, binance) == 50, "20·1,25=25 ⇒ ступень 50") && ok;
  ok = Check(NextRequestLimit(60, 1000, 1.25, binance) == 100, "60·1,25=75 ⇒ ступень 100") && ok;
  ok = Check(NextRequestLimit(400, 1000, 1.25, binance) == 500, "400·1,25=500 ⇒ ступень 500") && ok;
  ok = Check(NextRequestLimit(900, 1000, 1.25, binance) == 1000, "1125 > потолок ⇒ 1000") && ok;
  ok = Check(NextRequestLimit(113, 500, 1.25, {}) == 142, "Kraken: без ступеней, точное 142") && ok;
  ok = Check(NextRequestLimit(113, 120, 1.25, {}) == 120, "не больше потолка") && ok;
  ok = Check(NextRequestLimit(3, 1000, 1.25, {}) == 20, "не меньше min_limit (20)") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestQBarInvariance() && ok;
  ok = TestLevelsToReach() && ok;
  ok = TestSlippagePartialLevel() && ok;
  ok = TestWindowToyBook() && ok;
  ok = TestTruncatedNoExtrapolation() && ok;
  ok = TestApiCap() && ok;
  ok = TestNoInputs() && ok;
  ok = TestEwma() && ok;
  ok = TestHysteresis() && ok;
  ok = TestRequestLimit() && ok;
  if (!ok) return EXIT_FAILURE;
  std::cout << "depth_window_test: OK" << std::endl;
  return EXIT_SUCCESS;
}
