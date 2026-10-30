// ADR-082, шаг 1: множитель глубины агента по коэффициенту исполнения f. Чистая функция, числа детерминированы.
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

#include "domain/fill_depth_scale.hpp"

namespace {

using cex::market_data::domain::FillDepthScale;
using cex::market_data::domain::FillDepthScaleConfig;
using cex::market_data::domain::ResolveFillDepthScale;
using cex::market_data::domain::VenueFillKey;
using cex::market_data::domain::VenueFillSnapshot;
using cex::market_data::domain::VenueFillStat;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool Near(const double a, const double b, const double eps = 1e-12) { return std::fabs(a - b) <= eps; }

// Нет заявок или нечисловой вход ⇒ ничего не режем.
bool TestNeutralWithoutData() {
  const FillDepthScaleConfig c;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  return Check(FillDepthScale(0.2, 0.0, c) == 1.0, "no orders (n_eff = 0) must give 1") &&
         Check(FillDepthScale(0.2, -1.0, c) == 1.0, "negative n_eff must give 1") &&
         Check(FillDepthScale(nan, 10.0, c) == 1.0, "NaN f must give 1") &&
         Check(FillDepthScale(0.2, nan, c) == 1.0, "NaN n_eff must give 1");
}

// Много заявок ⇒ измеренный f (сжатие к единице исчезает): n_eff = 1e6, f = 0.3 ⇒ ≈ 0.3.
bool TestManyOrdersApproachMeasuredF() {
  const FillDepthScaleConfig c{0.1, 5.0};
  return Check(std::fabs(FillDepthScale(0.3, 1e6, c) - 0.3) < 1e-5, "many orders must approach the measured f");
}

// Сжатие: f = 0.2, n_eff = 20, n0 = 5 ⇒ (20·0.2 + 5) / 25 = 0.36.
bool TestShrinkage() {
  const FillDepthScaleConfig c{0.1, 5.0};
  return Check(Near(FillDepthScale(0.2, 20.0, c), 0.36), "shrinkage (n_eff*f + n0) / (n_eff + n0)");
}

// Нижняя граница: f = 0, n_eff большой ⇒ f_min; f_min = 0 ⇒ допускается 0 только при большом n_eff.
bool TestFloor() {
  const FillDepthScaleConfig c{0.1, 5.0};
  const FillDepthScaleConfig zero{0.0, 5.0};
  return Check(Near(FillDepthScale(0.0, 1e9, c), 0.1, 1e-6), "floor f_min must hold at f = 0") &&
         Check(FillDepthScale(0.0, 1e9, zero) < 1e-6, "f_min = 0 must allow ~0 at large n_eff");
}

// Множитель никогда не больше 1 и не меньше f_min; f вне [0,1] обрезается.
bool TestBounds() {
  const FillDepthScaleConfig c{0.1, 5.0};
  for (double f = -0.5; f <= 1.5; f += 0.1) {
    for (double n = 0.5; n <= 500.0; n *= 3.0) {
      const double s = FillDepthScale(f, n, c);
      if (!Check(s <= 1.0 && s >= 0.1, "scale must stay within [f_min, 1]")) return false;
    }
  }
  return Check(FillDepthScale(2.0, 10.0, c) == 1.0, "f above 1 must clamp to 1");
}

// Монотонность: при фиксированном n_eff больший f не даёт меньший множитель; при фиксированном f < 1 меньше заявок ⇒ ближе к 1.
bool TestMonotone() {
  const FillDepthScaleConfig c{0.1, 5.0};
  if (!Check(FillDepthScale(0.3, 30.0, c) <= FillDepthScale(0.6, 30.0, c), "scale must not decrease with f")) return false;
  return Check(FillDepthScale(0.3, 10.0, c) >= FillDepthScale(0.3, 40.0, c), "fewer orders must pull the scale toward 1");
}

// Возврат к 1 при затухании заявок: n_eff убывает по exp(-t/τ) ⇒ множитель растёт к 1.
bool TestRecoveryAsOrdersDecay() {
  const FillDepthScaleConfig c{0.1, 5.0};
  double prev = 0.0;
  for (int k = 0; k <= 6; ++k) {
    const double n_eff = 50.0 * std::exp(-static_cast<double>(k));
    const double s = FillDepthScale(0.3, n_eff, c);
    if (!Check(s >= prev, "scale must grow as n_eff decays")) return false;
    prev = s;
  }
  return Check(prev > 0.9, "after e^-6 decay the scale must be close to 1");
}

// Некорректная конфигурация не ломает формулу: n0 < 0 ⇒ 0 (без сжатия), f_min вне [0,1] обрезается.
bool TestBadConfigIsSafe() {
  const FillDepthScaleConfig neg{-1.0, -5.0};
  const double s = FillDepthScale(0.4, 10.0, neg);
  return Check(Near(s, 0.4), "negative n0 must be treated as 0 and f_min below 0 as 0") &&
         Check(FillDepthScale(0.0, 10.0, FillDepthScaleConfig{5.0, 5.0}) == 1.0, "f_min above 1 must clamp to 1");
}

// Поиск множителя по снимку: ключ venue|BASEQUOTE как у venues (BTCUSDT, ETHBTC); нет пары или биржи ⇒ 1.
bool TestResolveByKey() {
  const FillDepthScaleConfig c{0.1, 5.0, 1.0};  // ref_min = 1 ⇒ без нормировки, абсолютный f
  VenueFillSnapshot snap;
  snap[VenueFillKey("kraken", "BTCUSDT")] = VenueFillStat{0.2, 20.0};
  snap[VenueFillKey("binance", "ETHBTC")] = VenueFillStat{0.5, 40.0};
  if (!Check(Near(ResolveFillDepthScale(snap, "kraken", "BTC", "USDT", c), 0.36), "kraken BTC/USDT must resolve to 0.36")) return false;
  if (!Check(Near(ResolveFillDepthScale(snap, "binance", "ETH", "BTC", c), (40.0 * 0.5 + 5.0) / 45.0), "cross pair ETH/BTC must resolve by ETHBTC")) return false;
  return Check(ResolveFillDepthScale(snap, "okx", "BTC", "USDT", c) == 1.0, "venue without data must give 1") &&
         Check(ResolveFillDepthScale(snap, "kraken", "ETH", "USDT", c) == 1.0, "pair without data must give 1") &&
         Check(ResolveFillDepthScale(VenueFillSnapshot{}, "kraken", "BTC", "USDT", c) == 1.0, "empty snapshot must give 1");
}

// Нормировка на лучшую биржу актива: Binance 0.81 (много заявок) — опора, остальные пропорционально отставанию.
bool TestNormalizationToBestVenue() {
  const FillDepthScaleConfig c{0.1, 0.0, 0.5};  // n0 = 0: без сжатия, чтобы числа были чистыми
  VenueFillSnapshot snap;
  snap[VenueFillKey("binance", "BTCUSDT")] = VenueFillStat{0.81, 100.0};
  snap[VenueFillKey("okx", "BTCUSDT")] = VenueFillStat{0.729, 100.0};
  snap[VenueFillKey("kraken", "BTCUSDT")] = VenueFillStat{0.324, 100.0};
  snap[VenueFillKey("coinbase", "BTCUSDT")] = VenueFillStat{0.2916, 100.0};
  return Check(Near(ResolveFillDepthScale(snap, "binance", "BTC", "USDT", c), 1.0), "best venue must get 1") &&
         Check(Near(ResolveFillDepthScale(snap, "okx", "BTC", "USDT", c), 0.9), "okx = 0.729 / 0.81") &&
         Check(Near(ResolveFillDepthScale(snap, "kraken", "BTC", "USDT", c), 0.4), "kraken = 0.324 / 0.81") &&
         Check(Near(ResolveFillDepthScale(snap, "coinbase", "BTC", "USDT", c), 0.36), "coinbase = 0.2916 / 0.81");
}

// Общий шум: f упал у всех бирж одинаково (0.6 → 0.3 у всех) — относительные множители те же; при ref_min = 0.5 лучшая
// биржа с f 0.3 получает 0.3 / 0.5 = 0.6, а не 1 (защита «плохи все биржи сразу»).
bool TestRefMinGuardsAgainstAllPoor() {
  const FillDepthScaleConfig c{0.1, 0.0, 0.5};
  VenueFillSnapshot snap;
  snap[VenueFillKey("binance", "ETHUSDT")] = VenueFillStat{0.3, 100.0};
  snap[VenueFillKey("okx", "ETHUSDT")] = VenueFillStat{0.15, 100.0};
  return Check(Near(ResolveFillDepthScale(snap, "binance", "ETH", "USDT", c), 0.6), "all poor: best gets f / ref_min") &&
         Check(Near(ResolveFillDepthScale(snap, "okx", "ETH", "USDT", c), 0.3), "all poor: second gets 0.15 / 0.5");
}

// ref_min = 1 воспроизводит вариант «а» (абсолютный f) один в один; малый ref_min даёт чисто относительное сравнение.
bool TestRefMinExtremes() {
  VenueFillSnapshot snap;
  snap[VenueFillKey("a", "SOLUSDT")] = VenueFillStat{0.8, 50.0};
  snap[VenueFillKey("b", "SOLUSDT")] = VenueFillStat{0.4, 50.0};
  const FillDepthScaleConfig absolute{0.1, 5.0, 1.0};
  const FillDepthScaleConfig relative{0.1, 5.0, 0.0};
  if (!Check(Near(ResolveFillDepthScale(snap, "b", "SOL", "USDT", absolute), FillDepthScale(0.4, 50.0, absolute)),
             "ref_min = 1 must equal the absolute scale")) return false;
  const double ga = (50.0 * 0.8 + 5.0) / 55.0;
  const double gb = (50.0 * 0.4 + 5.0) / 55.0;
  return Check(Near(ResolveFillDepthScale(snap, "a", "SOL", "USDT", relative), 1.0), "relative: best gets 1") &&
         Check(Near(ResolveFillDepthScale(snap, "b", "SOL", "USDT", relative), gb / ga), "relative: second gets g_b / g_a");
}

// Биржи без данных по паре в опорный максимум не входят; единственная биржа с данными сравнивается с ref_min.
bool TestReferenceIgnoresVenuesWithoutData() {
  const FillDepthScaleConfig c{0.1, 0.0, 0.5};
  VenueFillSnapshot snap;
  snap[VenueFillKey("kraken", "SOLUSDT")] = VenueFillStat{0.2, 100.0};
  snap[VenueFillKey("binance", "ETHUSDT")] = VenueFillStat{0.9, 100.0};  // другая пара, на SOLUSDT не влияет
  return Check(Near(ResolveFillDepthScale(snap, "kraken", "SOL", "USDT", c), 0.4), "sole venue: f / ref_min = 0.2 / 0.5") &&
         Check(ResolveFillDepthScale(snap, "okx", "SOL", "USDT", c) == 1.0, "venue without data must give 1");
}

// Все f нулевые и опорной границы нет: сравнивать не с чем ⇒ 1, без деления на ноль. Нижняя граница f_min держится.
bool TestNoDivisionByZeroAndFloor() {
  VenueFillSnapshot snap;
  snap[VenueFillKey("a", "BTCUSDT")] = VenueFillStat{0.0, 100.0};
  snap[VenueFillKey("b", "BTCUSDT")] = VenueFillStat{0.0, 100.0};
  const FillDepthScaleConfig none{0.1, 0.0, 0.0};
  if (!Check(ResolveFillDepthScale(snap, "a", "BTC", "USDT", none) == 1.0, "all zero without ref_min must give 1")) return false;
  const FillDepthScaleConfig guarded{0.1, 0.0, 0.5};
  return Check(Near(ResolveFillDepthScale(snap, "a", "BTC", "USDT", guarded), 0.1), "f_min floor must hold when f = 0");
}

// Биржа почти без заявок (n_eff ≈ 0) после сжатия к единице имела бы g ≈ 1 и закрепила бы опору на 1: в опору она не входит.
bool TestReferenceIgnoresVenuesWithFewOrders() {
  const FillDepthScaleConfig c{0.1, 5.0, 0.0, 5.0};  // ref_min = 0, порог заявок 5
  VenueFillSnapshot snap;
  snap[VenueFillKey("quiet", "BTCUSDT")] = VenueFillStat{0.9, 0.01};
  snap[VenueFillKey("busy", "BTCUSDT")] = VenueFillStat{0.4, 100.0};
  const double busy = ResolveFillDepthScale(snap, "busy", "BTC", "USDT", c);
  const double quiet = ResolveFillDepthScale(snap, "quiet", "BTC", "USDT", c);
  return Check(Near(busy, 1.0), "the only trusted venue must be the reference and get 1, not ~0.4") &&
         Check(quiet <= 1.0 && quiet >= 0.1, "the quiet venue stays within bounds");
}

// Пары с общим суффиксом не путаются: ETHBTC и BTCUSDT в одном снимке независимы (точное сравнение ключа пары).
bool TestPairKeysDoNotCollide() {
  const FillDepthScaleConfig c{0.1, 0.0, 0.0, 0.0};
  VenueFillSnapshot snap;
  snap[VenueFillKey("a", "ETHBTC")] = VenueFillStat{0.2, 50.0};
  snap[VenueFillKey("b", "BTCUSDT")] = VenueFillStat{0.9, 50.0};
  snap[VenueFillKey("c", "BTCUSDT")] = VenueFillStat{0.45, 50.0};
  return Check(Near(ResolveFillDepthScale(snap, "a", "ETH", "BTC", c), 1.0), "ETHBTC sole venue must be its own reference") &&
         Check(Near(ResolveFillDepthScale(snap, "c", "BTC", "USDT", c), 0.5), "BTCUSDT reference must ignore ETHBTC");
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestNeutralWithoutData() && ok;
  ok = TestManyOrdersApproachMeasuredF() && ok;
  ok = TestShrinkage() && ok;
  ok = TestFloor() && ok;
  ok = TestBounds() && ok;
  ok = TestMonotone() && ok;
  ok = TestRecoveryAsOrdersDecay() && ok;
  ok = TestBadConfigIsSafe() && ok;
  ok = TestResolveByKey() && ok;
  ok = TestNormalizationToBestVenue() && ok;
  ok = TestRefMinGuardsAgainstAllPoor() && ok;
  ok = TestRefMinExtremes() && ok;
  ok = TestReferenceIgnoresVenuesWithoutData() && ok;
  ok = TestNoDivisionByZeroAndFloor() && ok;
  ok = TestReferenceIgnoresVenuesWithFewOrders() && ok;
  ok = TestPairKeysDoNotCollide() && ok;
  if (!ok) return 1;
  std::cout << "[PASS] fill_depth_scale_test" << std::endl;
  return 0;
}
