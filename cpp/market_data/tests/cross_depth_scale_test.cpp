// Приведение глубины кросс-пары к номиналу (ADR-064, дополнение 2026-10-29; OQ-F18v2-37).
//
// Главное свойство: один и тот же рынок, записанный в USDT (ETH/USDT) и в BTC (ETH/BTC при курсе BTC = 80 000 USDT), должен давать одну и ту же глубину
// ребра после приведения к номиналу. Без поправки α кросс-пары меньше ровно в P0_quote раз.
//
// Книга ETH/USDT: bids 2399, 2398; asks 2401, 2402 (mid 2400), объёмы по 1 ETH. Та же книга в BTC: цены / 80000 = 0.0299875, 0.029975, 0.0300125, 0.030025
// (точно представимы: масштаб 7).

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/agent_builder.hpp"
#include "domain/cross_depth_scale.hpp"
#include "domain/external_order_level.hpp"

using namespace cex::market_data::domain;
using cex::common::Decimal;

namespace {
int g_fail = 0;

bool CheckRel(double got, double want, double rel, const char* what) {
  const double scale = std::fabs(want) > 0.0 ? std::fabs(want) : 1.0;
  if (std::fabs(got - want) > rel * scale) {
    std::printf("FAIL %s: got %.12g want %.12g (rel %.3g)\n", what, got, want, std::fabs(got - want) / scale);
    ++g_fail;
    return false;
  }
  return true;
}

bool CheckTrue(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL %s\n", what);
    ++g_fail;
  }
  return cond;
}

ExternalOrderLevel Lvl(LevelSide side, long units, int scale, const char* base, const char* quote) {
  ExternalOrderLevel l;
  l.venue_id = "TestVenue";
  l.pair = std::string(base) + "/" + quote;
  l.base_asset = base;
  l.quote_asset = quote;
  l.side = side;
  l.price = Decimal{units, scale};
  l.quantity = Decimal{1, 0};
  l.fees_bps = 0.0;
  return l;
}

constexpr double kP0Btc = 80000.0;  // опорная цена BTC в USDT
constexpr double kP0Eth = 2400.0;   // опорная цена ETH в USDT

std::vector<ExternalOrderLevel> UsdtBook() {
  return {Lvl(LevelSide::kBid, 2399, 0, "ETH", "USDT"), Lvl(LevelSide::kBid, 2398, 0, "ETH", "USDT"),
          Lvl(LevelSide::kAsk, 2401, 0, "ETH", "USDT"), Lvl(LevelSide::kAsk, 2402, 0, "ETH", "USDT")};
}

std::vector<ExternalOrderLevel> BtcBook() {
  return {Lvl(LevelSide::kBid, 299875, 7, "ETH", "BTC"), Lvl(LevelSide::kBid, 299750, 7, "ETH", "BTC"),
          Lvl(LevelSide::kAsk, 300125, 7, "ETH", "BTC"), Lvl(LevelSide::kAsk, 300250, 7, "ETH", "BTC")};
}

// Тот же рынок в двух записях: после приведения глубина кросс-ребра равна глубине USDT-ребра; якорь и полка (‰) от единиц не зависят.
void TestSameMarketSameDepth() {
  // Опорная цена смещена относительно mid, чтобы якорь был ненулевым; множитель половины спреда включает полку (иначе она нулевая по умолчанию).
  const double p0_eth = kP0Eth * 1.001;
  AgentBuilderConfig usdt_cfg;
  usdt_cfg.reference_price = p0_eth;
  usdt_cfg.half_spread_mult = 1.0;
  AgentBuilderConfig cross_cfg;
  cross_cfg.reference_price = p0_eth / kP0Btc;  // как в market_data_uc: P0_base / P0_quote
  cross_cfg.half_spread_mult = 1.0;

  const QuoteAgent usdt = BuildQuoteAgent(UsdtBook(), usdt_cfg);
  const QuoteAgent cross = BuildQuoteAgent(BtcBook(), cross_cfg);
  CheckTrue(usdt.valid && cross.valid, "оба агента валидны");

  CheckTrue(std::fabs(usdt.anchor_pm) > 0.5, "якорь ненулевой (проверка не вырождена)");
  CheckTrue(usdt.dead_zone_pm > 0.1, "полка ненулевая (проверка не вырождена)");
  CheckRel(cross.anchor_pm, usdt.anchor_pm, 1e-9, "якорь (‰) не зависит от единиц");
  CheckRel(cross.dead_zone_pm, usdt.dead_zone_pm, 1e-9, "полка (‰) не зависит от единиц");

  // Без поправки (флаг выключен): множитель ровно 1 (битовое равенство), α кросс-пары меньше ровно в P0_quote раз — это и есть дефект.
  const double off = CrossDepthNumeraireFactor(false, "BTC", "USDT", kP0Btc);
  CheckTrue(off == 1.0, "флаг выключен ⇒ множитель ровно 1");
  CheckTrue(cross.depth * off != usdt.depth, "без поправки глубина кросс-пары НЕ равна глубине USDT-пары");
  CheckRel(cross.depth * off * kP0Btc, usdt.depth, 1e-9, "без поправки глубина кросс-пары меньше в P0_quote раз");

  // С поправкой: глубины совпадают.
  const double on = CrossDepthNumeraireFactor(true, "BTC", "USDT", kP0Btc);
  CheckRel(on, kP0Btc, 0.0, "флаг включён ⇒ множитель P0_quote");
  CheckRel(cross.depth * on, usdt.depth, 1e-9, "с поправкой глубина кросс-пары равна глубине USDT-пары");
}

// Пары с котировкой в номинале и граничные случаи не меняются.
void TestFactorEdgeCases() {
  CheckRel(CrossDepthNumeraireFactor(true, "USDT", "USDT", kP0Btc), 1.0, 0.0, "котировка в номинале ⇒ 1");
  CheckRel(CrossDepthNumeraireFactor(true, "", "USDT", kP0Btc), 1.0, 0.0, "пустая котировка (номинал) ⇒ 1");
  CheckRel(CrossDepthNumeraireFactor(true, "BTC", "USDT", 0.0), 1.0, 0.0, "нет цены ⇒ 1");
  CheckRel(CrossDepthNumeraireFactor(true, "BTC", "USDT", -5.0), 1.0, 0.0, "отрицательная цена ⇒ 1");
  CheckRel(CrossDepthNumeraireFactor(true, "BTC", "USDT", std::nan("")), 1.0, 0.0, "NaN ⇒ 1");
  CheckRel(CrossDepthNumeraireFactor(true, "BTC", "USDT", INFINITY), 1.0, 0.0, "бесконечность ⇒ 1");
  CheckRel(CrossDepthNumeraireFactor(false, "BTC", "USDT", kP0Btc), 1.0, 0.0, "выключено ⇒ 1");
}

// Две площадки одной кросс-пары разной ликвидности (объёмы 1 и 3 ETH на уровне): множитель одинаков, поэтому отношение их глубин (фильтр тонких
// min_ratio·max_depth по паре) то же, что до приведения, и сохраняется оценка через builder, а не умножение чисел-констант.
void TestRatioWithinPairPreserved() {
  AgentBuilderConfig cfg;
  cfg.reference_price = kP0Eth / kP0Btc;
  std::vector<ExternalOrderLevel> thin = BtcBook();
  std::vector<ExternalOrderLevel> thick = BtcBook();
  for (auto& lv : thick) lv.quantity = Decimal{3, 0};
  const QuoteAgent a_thin = BuildQuoteAgent(thin, cfg);
  const QuoteAgent a_thick = BuildQuoteAgent(thick, cfg);
  CheckTrue(a_thin.valid && a_thick.valid, "обе площадки валидны");
  const double f = CrossDepthNumeraireFactor(true, "BTC", "USDT", kP0Btc);
  CheckRel((a_thick.depth * f) / (a_thin.depth * f), a_thick.depth / a_thin.depth, 1e-12, "отношение глубин площадок пары не меняется");
  CheckRel(a_thick.depth / a_thin.depth, 3.0, 1e-9, "утроенный объём даёт утроенную глубину (ожидание builder)");
}
}  // namespace

int main() {
  TestSameMarketSameDepth();
  TestFactorEdgeCases();
  TestRatioWithinPairPreserved();
  if (g_fail == 0) std::printf("cross_depth_scale_test: OK\n");
  return g_fail == 0 ? 0 : 1;
}
