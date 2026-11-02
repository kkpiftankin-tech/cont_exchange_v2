// ADR-082 (шаг 0): юнит-тест чистых оценщиков ликвидности — оборот ленты ν (TapeRateEstimator) и коэффициент исполнения f
// (FillCoefficient). Числа детерминированы; допуск 1e-9 (чистая арифметика double).
#include <cmath>
#include <iostream>
#include <string>

#include "domain/liquidity_stats.hpp"

namespace {

using cex::venues::domain::FillCoefficient;
using cex::venues::domain::TapeRateEstimator;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool Near(const double a, const double b, const double eps = 1e-9) { return std::fabs(a - b) <= eps; }

// Первая выборка = сама скорость batch/dt; дальше экспоненциальное сглаживание a = 1 − exp(−dt/τ).
bool TestTapeFirstSampleAndSmoothing() {
  TapeRateEstimator t;
  t.Update(1000.0, 10.0, 900.0);  // 100/с
  if (!Check(t.initialized && t.samples == 1 && Near(t.rate_per_s, 100.0), "first sample must equal batch/dt")) return false;
  t.Update(0.0, 10.0, 900.0);  // тихое чтение — выборка 0
  const double a = 1.0 - std::exp(-10.0 / 900.0);
  return Check(Near(t.rate_per_s, 100.0 * (1.0 - a)) && t.samples == 2, "empty read must decay estimate by (1-a)");
}

// UpdateRate: готовая скорость вместо «объём / интервал»; вес выборки задаёт интервал, некорректный ввод игнорируется.
bool TestTapeUpdateRateWeightsByInterval() {
  TapeRateEstimator t;
  t.UpdateRate(100.0, 10.0, 900.0);
  t.UpdateRate(200.0, 10.0, 900.0);
  const double a = 1.0 - std::exp(-10.0 / 900.0);
  if (!Check(Near(t.rate_per_s, 100.0 + a * 100.0) && t.samples == 2, "UpdateRate must smooth with a = 1 - exp(-dt/tau)")) return false;
  t.UpdateRate(-5.0, 10.0, 900.0);
  t.UpdateRate(std::nan(""), 10.0, 900.0);
  t.UpdateRate(50.0, 0.0, 900.0);
  return Check(t.samples == 2, "bad UpdateRate input must be ignored");
}

// Тишина затухает монотонно и не уходит в минус.
bool TestTapeDecaysOnSilence() {
  TapeRateEstimator t;
  t.Update(500.0, 5.0, 300.0);
  double prev = t.rate_per_s;
  for (int i = 0; i < 200; ++i) {
    t.Update(0.0, 5.0, 300.0);
    if (!Check(t.rate_per_s <= prev && t.rate_per_s >= 0.0, "silence must decay monotonically and stay >= 0")) return false;
    prev = t.rate_per_s;
  }
  // Каждое тихое чтение умножает оценку на (1−a) = exp(−dt/τ) ⇒ после n чтений: 100·exp(−n·dt/τ).
  return Check(Near(t.rate_per_s, 100.0 * std::exp(-200.0 * 5.0 / 300.0), 1e-6),
               "after n silent reads the rate must equal r0 * exp(-n*dt/tau)");
}

// Некорректный вход игнорируется (dt<=0, τ<=0, отрицательный/NaN объём) — оценка не портится.
bool TestTapeIgnoresBadInput() {
  TapeRateEstimator t;
  t.Update(100.0, 0.0, 900.0);
  t.Update(100.0, 10.0, 0.0);
  t.Update(-1.0, 10.0, 900.0);
  t.Update(std::nan(""), 10.0, 900.0);
  return Check(!t.initialized && t.samples == 0 && t.rate_per_s == 0.0, "bad input must not create a sample");
}

// Нет заявок ⇒ f = 1 (нейтрально, ничего не режем), вес 0.
bool TestFillNeutralWithoutData() {
  FillCoefficient f;
  return Check(Near(f.Value(), 1.0) && f.Weight() == 0.0 && f.orders == 0, "no orders must give f = 1");
}

// Вес заявки — её запрошенный объём: большая неисполненная заявка тянет f сильнее маленькой исполненной.
bool TestFillVolumeWeighted() {
  FillCoefficient f;
  f.Add(1.0, 1.0, 100.0, 1800.0);   // исполнена полностью, объём 1
  f.Add(0.0, 3.0, 100.0, 1800.0);   // не исполнена, объём 3
  return Check(Near(f.Value(), 0.25) && Near(f.Weight(), 4.0) && f.orders == 2,
               "f must be filled/target weighted by requested volume");
}

// Переисполнение (filled > target) обрезается до target; f остаётся в [0,1].
bool TestFillClampedToTarget() {
  FillCoefficient f;
  f.Add(5.0, 2.0, 10.0, 1800.0);
  return Check(Near(f.Value(), 1.0), "filled above target must be clamped to target");
}

// Забывание: старые заявки теряют вес exp(−Δt/τ) — f следует за недавней историей.
bool TestFillForgetting() {
  FillCoefficient f;
  f.Add(0.0, 1.0, 0.0 + 1.0, 100.0);      // t=1: провал
  f.Add(1.0, 1.0, 1.0 + 100.0, 100.0);    // t=101: успех; провал взвешивается exp(-1)
  const double w_old = std::exp(-1.0);
  const double expect = 1.0 / (1.0 + w_old);
  return Check(Near(f.Value(), expect), "older orders must be down-weighted by exp(-dt/tau)");
}

// n_eff: число заявок с тем же забыванием, что у num/den; без новых заявок убывает как exp(−Δt/τ) при чтении.
bool TestFillOrdersEff() {
  FillCoefficient f;
  if (!Check(Near(f.OrdersEff(100.0, 1800.0), 0.0), "no orders ⇒ n_eff = 0")) return false;
  f.Add(1.0, 1.0, 10.0, 100.0);
  f.Add(0.0, 1.0, 110.0, 100.0);  // первая заявка забыта в exp(-1) раз
  const double d = std::exp(-1.0);
  if (!Check(Near(f.n_eff, d + 1.0), "n_eff must decay with the same factor as num/den")) return false;
  // Чтение через 200 с после последней заявки: ещё exp(-2).
  if (!Check(Near(f.OrdersEff(310.0, 100.0), (d + 1.0) * std::exp(-2.0)), "OrdersEff must decay from the last order")) return false;
  return Check(Near(f.OrdersEff(110.0, 100.0), d + 1.0) && Near(f.OrdersEff(50.0, 100.0), d + 1.0),
               "OrdersEff at or before the last order must not decay");
}

// WeightAt: вес убывает как exp(−Δt/τ) от последней заявки; до неё и в её момент не убывает.
bool TestFillWeightAt() {
  FillCoefficient f;
  f.Add(1.0, 4.0, 10.0, 100.0);
  return Check(Near(f.WeightAt(10.0, 100.0), 4.0) && Near(f.WeightAt(5.0, 100.0), 4.0), "weight must not decay at or before the last order") &&
         Check(Near(f.WeightAt(110.0, 100.0), 4.0 * std::exp(-1.0)), "weight must decay as exp(-dt/tau)");
}

// Seed (затравка после перезапуска): состояние продолжается, а не начинается с нуля; следующая заявка забывает затравку от now_s.
bool TestSeeds() {
  TapeRateEstimator t;
  t.Seed(100.0, 30);
  if (!Check(t.initialized && Near(t.rate_per_s, 100.0) && t.samples == 30 && t.reads == 0, "tape seed must set rate and samples only")) return false;
  t.UpdateRate(200.0, 10.0, 900.0);
  const double a = 1.0 - std::exp(-10.0 / 900.0);
  if (!Check(Near(t.rate_per_s, 100.0 + a * 100.0), "after seeding the first sample must be smoothed, not taken raw")) return false;
  TapeRateEstimator bad;
  bad.Seed(-1.0, 5);
  bad.Seed(10.0, 0);
  if (!Check(!bad.initialized, "bad tape seed must be ignored")) return false;

  FillCoefficient f;
  f.Seed(2.0, 4.0, 3.0, 40, 1000.0);
  if (!Check(Near(f.Value(), 0.5) && Near(f.Weight(), 4.0) && Near(f.OrdersEff(1000.0, 100.0), 3.0) && f.orders == 40,
             "fill seed must restore f, weight, n_eff and orders")) return false;
  f.Add(0.0, 4.0, 1100.0, 100.0);  // через τ: затравка забыта в exp(-1) раз
  const double d = std::exp(-1.0);
  if (!Check(Near(f.Value(), (2.0 * d) / (4.0 * d + 4.0)) && f.orders == 41, "next order must decay the seed from its timestamp")) return false;
  FillCoefficient bad_fill;
  bad_fill.Seed(5.0, 4.0, 1.0, 10, 1.0);   // num > den обрезается до den
  FillCoefficient none;
  none.Seed(1.0, 0.0, 1.0, 10, 1.0);       // den = 0 — игнор
  return Check(Near(bad_fill.Value(), 1.0), "num above den must clamp f to 1") &&
         Check(none.orders == 0, "fill seed with zero weight must be ignored");
}

// Некорректные заявки (target<=0, filled<0, NaN) не засчитываются.
bool TestFillIgnoresBadInput() {
  FillCoefficient f;
  f.Add(1.0, 0.0, 10.0, 1800.0);
  f.Add(-1.0, 1.0, 10.0, 1800.0);
  f.Add(std::nan(""), 1.0, 10.0, 1800.0);
  return Check(f.orders == 0 && Near(f.Value(), 1.0), "bad orders must not be counted");
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestTapeFirstSampleAndSmoothing() && ok;
  ok = TestTapeUpdateRateWeightsByInterval() && ok;
  ok = TestTapeDecaysOnSilence() && ok;
  ok = TestTapeIgnoresBadInput() && ok;
  ok = TestFillNeutralWithoutData() && ok;
  ok = TestFillVolumeWeighted() && ok;
  ok = TestFillClampedToTarget() && ok;
  ok = TestFillForgetting() && ok;
  ok = TestFillOrdersEff() && ok;
  ok = TestFillWeightAt() && ok;
  ok = TestSeeds() && ok;
  ok = TestFillIgnoresBadInput() && ok;
  if (!ok) return 1;
  std::cout << "[PASS] liquidity_stats_test" << std::endl;
  return 0;
}
