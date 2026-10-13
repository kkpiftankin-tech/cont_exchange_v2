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
  // Полка по умолчанию НУЛЕВАЯ: half_spread_mult=0 + commission_in_polka=false
  // (решение владельца 2026-09-16). Прежнее 10.20034 — из эпохи ADR-055 (½спред+fee),
  // устарело после обнуления полки; было красным. Age-член тоже 0 (k=0 по умолчанию).
  close(a.dead_zone_pm, 0.0, 1e-9, "dead_zone c (полка нулевая по умолчанию)");

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

  // ADR-075 (этап 4): age-aware полка добавляет член k·σ·√(age_sec), ‰.
  // Проверяем ДЕЛЬТУ к базовой полке — не завися от её абсолютного значения.
  {
    AgentBuilderConfig base = cfg;  // age_dead_zone_k=0 ⇒ член нулевой (деф)
    const double base_c = BuildQuoteAgent(book, base).dead_zone_pm;
    // k=0 (деф) не меняет полку
    close(BuildQuoteAgent(book, base).dead_zone_pm, base_c, 1e-9, "age-полка off (k=0)");
    // k=1, σ=2‰, age=4000ms (√4=2) ⇒ +1·2·2 = 4‰
    AgentBuilderConfig ca = cfg;
    ca.age_dead_zone_k = 1.0; ca.sigma_pm = 2.0; ca.snapshot_age_ms = 4000.0;
    close(BuildQuoteAgent(book, ca).dead_zone_pm, base_c + 4.0, 1e-6, "age-полка +k·σ·√age=4");
    // σ=0 ⇒ член 0 (нет оценки волатильности — не штрафуем)
    AgentBuilderConfig cs = ca; cs.sigma_pm = 0.0;
    close(BuildQuoteAgent(book, cs).dead_zone_pm, base_c, 1e-9, "age-полка σ=0 ⇒ 0");
    // age=0 ⇒ член 0 (свежий снимок)
    AgentBuilderConfig cz = ca; cz.snapshot_age_ms = 0.0;
    close(BuildQuoteAgent(book, cz).dead_zone_pm, base_c, 1e-9, "age-полка age=0 ⇒ 0");
  }

  if (g_fail == 0) { std::printf("agent_builder_test: OK\n"); return 0; }
  std::printf("agent_builder_test: %d FAIL\n", g_fail);
  return 1;
}
