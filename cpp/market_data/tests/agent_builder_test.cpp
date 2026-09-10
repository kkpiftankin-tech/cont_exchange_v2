// T-CEA-104: unit-тест book-derived quote-агента (ADR-055 §A1). Golden нет (вывод α/c
// из книги — формула), поэтому сверяем с аналитически посчитанными значениями на
// синтетической книге. Допуск 1e-3 (impl использует те же std::log).
//
// Книга (BTC/USDT): bids (99,1),(98,1); asks (101,1),(102,1); fee 2 bps; θ=0.5; P0=99.
//   mid = 100
//   α_ext(ask) = min(101/|1000ln(101/100)|, 203/|1000ln(102/100)|) = min(10.1504,10.2512)=10.1504
//   α_ext(bid) = min(99/|1000ln(99/100)|,  197/|1000ln(98/100)|)  = min(9.8504, 9.7512)=9.7512
//   α = 0.5·min(9.7512,10.1504) = 4.8756
//   c = fee(0.2‰) + ½·|1000ln(101/99)| = 0.2 + 10.0003 = 10.2003 ‰
//   anchor = 1000·ln(100/99) = 10.0503 ‰

#include <cmath>
#include <cstdio>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/agent_builder.hpp"
#include "domain/external_order_level.hpp"

using namespace cex::market_data::domain;
using cex::common::Decimal;

namespace {
int g_fail = 0;
bool close(double a, double b, double tol, const char* what) {
  if (std::fabs(a - b) > tol) {
    std::printf("FAIL %s: got %.10g want %.10g (|Δ|=%.3g)\n", what, a, b, std::fabs(a - b));
    ++g_fail;
    return false;
  }
  return true;
}
ExternalOrderLevel Lvl(LevelSide side, long price, long qty, double fee_bps) {
  ExternalOrderLevel l;
  l.venue_id = "TestVenue"; l.pair = "BTC/USDT"; l.base_asset = "BTC"; l.quote_asset = "USDT";
  l.side = side; l.price = Decimal{price, 0}; l.quantity = Decimal{qty, 0}; l.fees_bps = fee_bps;
  return l;
}
}  // namespace

int main() {
  std::vector<ExternalOrderLevel> book = {
      Lvl(LevelSide::kBid, 99, 1, 2.0), Lvl(LevelSide::kBid, 98, 1, 2.0),
      Lvl(LevelSide::kAsk, 101, 1, 2.0), Lvl(LevelSide::kAsk, 102, 1, 2.0),
  };
  AgentBuilderConfig cfg; cfg.theta = 0.5; cfg.reference_price = 99.0;
  QuoteAgent a = BuildQuoteAgent(book, cfg);

  if (!a.valid) { std::printf("FAIL: agent invalid (%s)\n", a.reason.c_str()); ++g_fail; }
  close(a.anchor_pm, 10.05034, 1e-3, "anchor_pm");
  close(a.depth, 4.875595, 1e-3, "depth α");
  close(a.dead_zone_pm, 10.20034, 1e-3, "dead_zone c");

  // θ масштабирует глубину линейно
  AgentBuilderConfig cfg2 = cfg; cfg2.theta = 0.3;
  close(BuildQuoteAgent(book, cfg2).depth, 0.3 / 0.5 * a.depth, 1e-6, "θ scaling");

  // reference_price ≤ 0 → anchor 0 (вырожденный)
  AgentBuilderConfig cfg3 = cfg; cfg3.reference_price = 0.0;
  close(BuildQuoteAgent(book, cfg3).anchor_pm, 0.0, 1e-9, "anchor@ref0");

  // односторонняя книга → невалиден
  std::vector<ExternalOrderLevel> one_sided = {Lvl(LevelSide::kBid, 99, 1, 2.0)};
  if (BuildQuoteAgent(one_sided, cfg).valid) {
    std::printf("FAIL: односторонняя книга должна быть невалидна\n"); ++g_fail;
  }

  if (g_fail == 0) { std::printf("agent_builder_test: OK\n"); return 0; }
  std::printf("agent_builder_test: %d FAIL\n", g_fail);
  return 1;
}
