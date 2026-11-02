#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "app/depth_window_controller.hpp"
#include "domain/depth_window.hpp"

using cex::common::Decimal;
using cex::venues::app::DepthWindowController;
using cex::venues::app::DepthWindowControllerConfig;
using cex::venues::domain::VenueBookLevel;
using cex::venues::domain::VenueRawSnapshot;

namespace {

bool Check(bool cond, const std::string& msg) {
  if (!cond) std::cerr << "[FAIL] " << msg << std::endl;
  return cond;
}

constexpr std::int32_t kPriceScale = 8;
constexpr std::int32_t kQtyScale = 6;

Decimal Px(double v) { return Decimal{static_cast<std::int64_t>(std::llround(v * 1e8)), kPriceScale}; }
Decimal Qty(double v) { return Decimal{static_cast<std::int64_t>(std::llround(v * 1e6)), kQtyScale}; }

// Симметричная книга: уровень i лежит на step·(i+1) от price0, стоимость уровня ≈ level_usd_in_quote.
VenueRawSnapshot MakeSnapshot(const std::string& venue, const std::string& symbol,
                              const std::string& base, const std::string& quote, double price0,
                              double step, std::size_t n, double level_notional, double volume_base) {
  VenueRawSnapshot s;
  s.venue_id = venue;
  s.venue_symbol = symbol;
  s.instrument.set_base(base);
  s.instrument.set_quote(quote);
  s.instrument.set_symbol(base + "/" + quote);
  for (std::size_t i = 0; i < n; ++i) {
    const double pb = price0 - step * static_cast<double>(i + 1);
    const double pa = price0 + step * static_cast<double>(i + 1);
    s.bids.push_back({Px(pb), Qty(level_notional / pb)});
    s.asks.push_back({Px(pa), Qty(level_notional / pa)});
  }
  s.volume_24h = Qty(volume_base);
  return s;
}

DepthWindowControllerConfig EnabledCfg() {
  DepthWindowControllerConfig c;
  c.enabled = true;
  c.api_cap_by_venue = {{"binance", 1000}, {"kraken", 500}};
  return c;
}

DepthWindowController::SigmaProvider FixedSigma(std::optional<double> s) {
  return [s](const std::string&) { return s; };
}

bool TestDisabledIsNoop() {
  DepthWindowControllerConfig c;  // enabled=false
  DepthWindowController ctl(c, FixedSigma(3.7e-5));
  auto snap = MakeSnapshot("binance", "BTCUSDT", "BTC", "USDT", 100.0, 0.01, 300, 1001.0, 5000.0);
  const auto out = ctl.Apply(&snap, 20, 1000);
  bool ok = Check(!out.applied && out.tags.empty(), "выключено ⇒ нет эффекта и тегов");
  ok = Check(snap.bids.size() == 300 && snap.asks.size() == 300, "снимок не тронут") && ok;
  ok = Check(ctl.RequestLimit("binance", "BTCUSDT") == 0, "выключено ⇒ лимит 0 (прежний путь)") && ok;
  return ok;
}

bool TestOverrideTrimsAndAdaptsLimit() {
  auto c = EnabledCfg();
  c.q_bar_override_usd["binance|BTCUSDT"] = 5000.0;  // V_cal = 10·5000 = 50 000
  DepthWindowController ctl(c, FixedSigma(std::nullopt));
  bool ok = Check(ctl.RequestLimit("binance", "BTCUSDT") == 1000, "первый запрос — на потолке API");
  auto snap = MakeSnapshot("binance", "BTCUSDT", "BTC", "USDT", 100.0, 0.01, 300, 1001.0, 5000.0);
  const auto out = ctl.Apply(&snap, 20, 1000);
  ok = Check(out.applied && out.result.n == 50, "V_cal=50 000 при уровнях по 1001 ⇒ n=50") && ok;
  ok = Check(snap.bids.size() == 50 && snap.asks.size() == 50, "снимок обрезан до n обеих сторон") && ok;
  ok = Check(out.tags.at("depth_window.applied") == "1" && out.tags.at("depth_window.n") == "50",
             "теги: applied, n") && ok;
  ok = Check(out.tags.at("depth_window.truncated") == "0", "стакан глубже окна — не truncated") && ok;
  ok = Check(ctl.RequestLimit("binance", "BTCUSDT") == 100, "n=50 ⇒ 63 ⇒ ступень Binance 100") && ok;
  ok = Check(ctl.RequestLimit("kraken", "BTCUSDT") == 500,
             "другая площадка — свой слот, первый запрос на её потолке (Kraken 500)") && ok;
  return ok;
}

bool TestNoInputsFallsBackToLegacy() {
  auto c = EnabledCfg();
  DepthWindowController ctl(c, FixedSigma(std::nullopt));  // нет σ и нет явного q̄
  auto snap = MakeSnapshot("binance", "ETHUSDT", "ETH", "USDT", 100.0, 0.01, 300, 1001.0, 5000.0);
  const auto out = ctl.Apply(&snap, 20, 1000);
  bool ok = Check(!out.applied && out.tags.at("depth_window.no_inputs") == "1", "нет входов ⇒ no_inputs");
  ok = Check(snap.bids.size() == 20 && snap.asks.size() == 20, "обрезано до прежних 20 уровней") && ok;
  ok = Check(ctl.RequestLimit("binance", "ETHUSDT") == 0, "no_inputs ⇒ запрос прежним путём") && ok;
  auto zero_vol = MakeSnapshot("binance", "SOLUSDT", "SOL", "USDT", 100.0, 0.01, 300, 1001.0, 0.0);
  DepthWindowController ctl2(c, FixedSigma(3.7e-5));
  ok = Check(!ctl2.Apply(&zero_vol, 20, 1000).applied, "нулевой суточный объём ⇒ no_inputs") && ok;
  return ok;
}

bool TestFormulaPathUsesInvariance() {
  auto c = EnabledCfg();
  const double sigma_s = 3.7e-5;
  DepthWindowController ctl(c, FixedSigma(sigma_s));
  // 24ч объём 20 000 BTC × mid 100 = 2e6 USD.
  auto snap = MakeSnapshot("binance", "BTCUSDT", "BTC", "USDT", 100.0, 0.01, 300, 1001.0, 20000.0);
  const auto out = ctl.Apply(&snap, 20, 1000);
  const double v_usd = 20000.0 * 100.0;
  const double q_expect = cex::venues::domain::QBarFromInvariance(
      v_usd, sigma_s * std::sqrt(86400.0), c.window);
  bool ok = Check(out.applied, "формульный путь применён");
  ok = Check(std::fabs(out.result.q_bar_usd - q_expect) <= 1e-9 * q_expect, "q̄ = формула инвариантности") && ok;
  ok = Check(std::fabs(out.result.v_cal_usd - 10.0 * q_expect) <= 1e-9 * q_expect, "V_cal = 10·q̄") && ok;
  return ok;
}

bool TestCrossPairConvertedToUsd() {
  auto c = EnabledCfg();
  const double sigma_s = 3.7e-5;
  DepthWindowController ctl(c, FixedSigma(sigma_s));
  // Сначала BTC/USDT на той же площадке — даёт курс BTC в USDT.
  auto btc = MakeSnapshot("binance", "BTCUSDT", "BTC", "USDT", 100000.0, 1.0, 50, 1001.0, 10.0);
  (void)ctl.Apply(&btc, 20, 1000);
  // ETH/BTC: цена 0.05 BTC, оборот 4 ETH ⇒ V$ = 4 · 0.05 · 100 000 = 2e4 (окно умещается в 400 уровней).
  auto eth = MakeSnapshot("binance", "ETHBTC", "ETH", "BTC", 0.05, 1e-6, 400, 0.01, 4.0);
  const auto out = ctl.Apply(&eth, 20, 1000);
  const double q_expect = cex::venues::domain::QBarFromInvariance(
      2.0e4, sigma_s * std::sqrt(86400.0), c.window);
  bool ok = Check(out.applied, "кросс-пара применена при известном курсе BTC");
  ok = Check(std::fabs(out.result.q_bar_usd - q_expect) <= 1e-6 * q_expect, "q̄ по V$ в долларах") && ok;
  // Стоимость уровня 0.01 BTC = 1000 USD ⇒ n по долларам совпадает с n по V_cal/1000.
  const double expect_n = std::ceil(out.result.v_cal_usd / 1000.0);
  ok = Check(!out.result.truncated && std::fabs(static_cast<double>(out.result.n) - expect_n) <= 1.0,
             "n считается в долларах, а не в BTC") && ok;
  // Без курса BTC окно для кросс-пары не применяется, даже с явным q̄.
  auto c2 = EnabledCfg();
  c2.q_bar_override_usd["binance|ETHBTC"] = 5000.0;
  DepthWindowController ctl2(c2, FixedSigma(sigma_s));
  auto eth2 = MakeSnapshot("binance", "ETHBTC", "ETH", "BTC", 0.05, 1e-6, 400, 0.01, 4.0);
  ok = Check(!ctl2.Apply(&eth2, 20, 1000).applied, "нет курса котируемой валюты ⇒ no_inputs") && ok;
  return ok;
}

bool TestRequestLimitedForcesCapThenSettles() {
  auto c = EnabledCfg();
  c.q_bar_override_usd["binance|BTCUSDT"] = 50000.0;  // V_cal = 500 000 ⇒ нужно ≈500 уровней по 1001
  DepthWindowController ctl(c, FixedSigma(std::nullopt));
  // Просили 63, площадка отдала ровно 63 — окно обрезал НАШ лимит, а не тонкий стакан.
  auto snap = MakeSnapshot("binance", "BTCUSDT", "BTC", "USDT", 100.0, 0.01, 63, 1001.0, 5000.0);
  const auto out = ctl.Apply(&snap, 20, 63);
  bool ok = Check(out.applied && out.tags.at("depth_window.request_limited") == "1",
                  "visible==requested и объём не набран ⇒ request_limited");
  ok = Check(out.tags.at("depth_window.truncated") == "0", "это не усечение стакана (truncated=0)") && ok;
  ok = Check(ctl.RequestLimit("binance", "BTCUSDT") == 1000, "следующий запрос — на потолке API") && ok;
  // Площадка отдала 300 из 1000 запрошенных: стакан реально тоньше окна.
  auto thin = MakeSnapshot("binance", "BTCUSDT", "BTC", "USDT", 100.0, 0.01, 300, 1001.0, 5000.0);
  const auto out2 = ctl.Apply(&thin, 20, 1000);
  ok = Check(out2.tags.at("depth_window.truncated") == "1" && out2.tags.at("depth_window.request_limited") == "0",
             "visible<requested и объём не набран ⇒ truncated, не request_limited") && ok;
  ok = Check(ctl.RequestLimit("binance", "BTCUSDT") == 500, "форс потолка снят: 300·1,25=375 ⇒ ступень 500") && ok;
  return ok;
}

bool TestNoInputsResetsEwma() {
  auto c = EnabledCfg();
  auto sigma = std::make_shared<std::optional<double>>(3.7e-5);
  DepthWindowController ctl(c, [sigma](const std::string&) { return *sigma; });
  auto s1 = MakeSnapshot("binance", "BTCUSDT", "BTC", "USDT", 100.0, 0.01, 300, 1001.0, 20000.0);
  (void)ctl.Apply(&s1, 20, 1000);
  *sigma = std::nullopt;  // σ пропала ⇒ no_inputs
  auto s2 = MakeSnapshot("binance", "BTCUSDT", "BTC", "USDT", 100.0, 0.01, 300, 1001.0, 20000.0);
  bool ok = Check(!ctl.Apply(&s2, 20, 1000).applied, "σ нет ⇒ no_inputs");
  *sigma = 3.7e-5;
  auto s3 = MakeSnapshot("binance", "BTCUSDT", "BTC", "USDT", 100.0, 0.01, 300, 1001.0, 5000.0);  // другой объём
  const auto out = ctl.Apply(&s3, 20, 1000);
  ok = Check(out.applied && out.tags.at("depth_window.v_cal_usd") == out.tags.at("depth_window.v_cal_raw_usd"),
             "после no_inputs EWMA начинается заново (V_cal == сырому)") && ok;
  return ok;
}

bool TestWindowVolumeFieldWins() {
  auto c = EnabledCfg();
  DepthWindowController ctl(c, FixedSigma(3.7e-5));
  // OKX/Kraken: общий volume_24h = 0 (тикер Binance-путём 404), объём окна — в отдельном поле.
  auto snap = MakeSnapshot("okx", "BTCUSDT", "BTC", "USDT", 100.0, 0.01, 300, 1001.0, 0.0);
  snap.window_volume_24h = Qty(20000.0);
  const auto out = ctl.Apply(&snap, 20, 400);
  const double q_expect = cex::venues::domain::QBarFromInvariance(
      20000.0 * 100.0, 3.7e-5 * std::sqrt(86400.0), c.window);
  bool ok = Check(out.applied, "объём окна из window_volume_24h ⇒ окно применено");
  ok = Check(std::fabs(out.result.q_bar_usd - q_expect) <= 1e-9 * q_expect, "q̄ по объёму окна") && ok;
  return ok;
}

bool TestEnvBounds() {
  setenv("CE_DEPTH_WINDOW_ENABLED", "1", 1);
  setenv("CE_DEPTH_HEADROOM", "1e308", 1);
  setenv("CE_DEPTH_DEFAULT_API_CAP", "1e300", 1);
  setenv("CE_DEPTH_K", "-5", 1);
  const auto c = DepthWindowControllerConfig::FromEnv();
  bool ok = Check(c.enabled, "флаг включения читается");
  ok = Check(c.headroom == 1.25, "запас вне [1;10] ⇒ дефолт") && ok;
  ok = Check(c.default_api_cap == 100, "потолок вне [1;1e6] ⇒ дефолт (без UB при приведении)") && ok;
  ok = Check(c.window.k == 10.0, "k вне [0,1;1000] ⇒ дефолт") && ok;
  ok = Check(c.api_cap_by_venue.at("coinbase") >= 100000, "Coinbase отдаёт весь стакан ⇒ потолок не ограничивает") && ok;
  unsetenv("CE_DEPTH_WINDOW_ENABLED");
  unsetenv("CE_DEPTH_HEADROOM");
  unsetenv("CE_DEPTH_DEFAULT_API_CAP");
  unsetenv("CE_DEPTH_K");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestDisabledIsNoop() && ok;
  ok = TestOverrideTrimsAndAdaptsLimit() && ok;
  ok = TestNoInputsFallsBackToLegacy() && ok;
  ok = TestFormulaPathUsesInvariance() && ok;
  ok = TestCrossPairConvertedToUsd() && ok;
  ok = TestRequestLimitedForcesCapThenSettles() && ok;
  ok = TestNoInputsResetsEwma() && ok;
  ok = TestWindowVolumeFieldWins() && ok;
  ok = TestEnvBounds() && ok;
  if (!ok) return EXIT_FAILURE;
  std::cout << "depth_window_controller_test: OK" << std::endl;
  return EXIT_SUCCESS;
}
