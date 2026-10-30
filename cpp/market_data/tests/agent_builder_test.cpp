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

  // ADR-079: компоненты полки для диагностики «почему нет потока»: c = комиссия + спред + возраст.
  {
    AgentBuilderConfig ca = cfg;
    ca.age_dead_zone_k = 1.0; ca.sigma_pm = 2.0; ca.snapshot_age_ms = 4000.0;     // age-член = 4‰
    const QuoteAgent q0 = BuildQuoteAgent(book, cfg);
    close(q0.dz_age_pm, 0.0, 1e-12, "компонент: age=0 при k=0");
    close(q0.dz_commission_pm + q0.dz_spread_pm + q0.dz_age_pm, q0.dead_zone_pm, 1e-9, "компоненты суммируются в полку (k=0)");
    const QuoteAgent q1 = BuildQuoteAgent(book, ca);
    close(q1.dz_age_pm, 4.0, 1e-6, "компонент: age-член = k·σ·√age = 4");
    close(q1.dz_commission_pm + q1.dz_spread_pm + q1.dz_age_pm, q1.dead_zone_pm, 1e-9, "компоненты суммируются в полку (k=1)");
    // комиссия в полке и множитель спреда попадают в свои компоненты
    AgentBuilderConfig cf = ca;
    cf.commission_in_polka = true; cf.half_spread_mult = 1.0; cf.taker_fee_bps_override = 10.0;   // 10 б.п. = 1‰
    const QuoteAgent q2 = BuildQuoteAgent(book, cf);
    close(q2.dz_commission_pm, 1.0, 1e-9, "компонент: комиссия 10 б.п. = 1‰");
    if (!(q2.dz_spread_pm > 0.0)) { std::printf("FAIL: компонент спреда должен быть > 0 при множителе 1\n"); ++g_fail; }
    close(q2.dz_commission_pm + q2.dz_spread_pm + q2.dz_age_pm, q2.dead_zone_pm, 1e-9, "компоненты суммируются в полку (все три)");
  }

  // ADR-077 этап B: M5 — регрессия предельного сдвига на сетке объёма. Конвенция CE:
  // уровень по цене (предельная), сдвиг в ‰, α в ед. котируемой валюты на ‰.
  {
    // Линейная книга: на кумулятивном объёме D предельный сдвиг = h + D/κ (κ=2e6, шаг 200).
    auto frac_lvl = [](LevelSide side, double price, double qty) {
      ExternalOrderLevel l;
      l.venue_id = "V"; l.pair = "BTC/USDT"; l.base_asset = "BTC"; l.quote_asset = "USDT";
      l.side = side;
      l.price = Decimal{static_cast<std::int64_t>(std::llround(price * 1e8)), 8};
      l.quantity = Decimal{static_cast<std::int64_t>(std::llround(qty * 1e8)), 8};
      return l;
    };
    auto linear_book = [&](double h, int n_levels, double kappa) {
      std::vector<ExternalOrderLevel> b;
      for (int k = 1; k <= n_levels; ++k) {
        const double x = h + 200.0 * k / kappa;
        const double pa = 100.0 * (1.0 + x), pb = 100.0 * (1.0 - x);
        b.push_back(frac_lvl(LevelSide::kAsk, pa, 200.0 / pa));
        b.push_back(frac_lvl(LevelSide::kBid, pb, 200.0 / pb));
      }
      return b;
    };
    AgentBuilderConfig m5; m5.theta = 0.5; m5.reference_price = 100.0;
    m5.alpha_method = 5; m5.depth_window_quote = 20000.0;

    // T-M5-LIN, s=0: M4 и M5 совпадают с κ·1e-3 = 2000 (допуск 3 % на кривизну ln).
    const auto lin = linear_book(0.0, 100, 2.0e6);
    const QuoteAgent q5 = BuildQuoteAgent(lin, m5);
    close(q5.alpha_m5 / 2000.0, 1.0, 0.03, "M5 линейная книга: α≈κ·1e-3");
    close(q5.alpha_m4 / 2000.0, 1.0, 0.03, "M4 линейная книга: α≈κ·1e-3");
    close(q5.depth, 0.5 * q5.alpha_m5, 1e-9, "depth = θ·α_M5");
    close(q5.m5_intercept_pm, 0.0, 0.1, "свободный член≈0 при s=0");
    if (q5.alpha_method_used != 5 || !q5.m5_fallback.empty()) {
      std::printf("FAIL: M5 на линейной книге должен примениться (fallback=%s)\n", q5.m5_fallback.c_str());
      ++g_fail;
    }

    // Определённость: два вызова дают побитно один результат (replay).
    if (BuildQuoteAgent(lin, m5).depth != q5.depth) {
      std::printf("FAIL: M5 недетерминирован\n"); ++g_fail;
    }

    // Со спредом h=1‰: M4≈200/1.1 (спред в наклоне), M5≈2000 (спред в свободном члене), полка M5 = ½спред.
    const auto sp = linear_book(1.0e-3, 100, 2.0e6);
    const QuoteAgent s5 = BuildQuoteAgent(sp, m5);
    close(s5.alpha_m5 / 2000.0, 1.0, 0.03, "M5 со спредом: α≈2000");
    close(s5.alpha_m4, 200.0 / 1.1, 2.0, "M4 со спредом≈181.8 (спред съедает наклон)");
    close(s5.m5_intercept_pm, 1.0, 0.15, "свободный член≈½спреда (1‰)");
    // По умолчанию полки нет (решение владельца 2026-10-15): спред остаётся только в свободном члене.
    close(s5.dead_zone_pm, 0.0, 1e-9, "M5: по умолчанию полка нулевая");
    // Механизм m·½спред работает при явном m=1. Лучшие уровни 100·(1±1.1e-3) ⇒ 500·ln(1.0011/0.9989) ‰.
    AgentBuilderConfig m5h = m5; m5h.m5_half_spread_mult = 1.0;
    close(BuildQuoteAgent(sp, m5h).dead_zone_pm, 500.0 * std::log(1.0011 / 0.9989), 0.01,
          "M5 при m=1: полка = ½спред");
    AgentBuilderConfig m4 = m5; m4.alpha_method = 4;
    close(BuildQuoteAgent(sp, m4).dead_zone_pm, 0.0, 1e-9, "M4: полка нулевая (прежнее поведение)");

    // Откат: сторона покрывает <50 % V_cal ⇒ M4 с причиной.
    AgentBuilderConfig cov = m5; cov.depth_window_quote = 1.0e6;
    const QuoteAgent qc = BuildQuoteAgent(lin, cov);
    if (qc.alpha_method_used != 4 || qc.m5_fallback.find("coverage") == std::string::npos) {
      std::printf("FAIL: coverage-откат (used=%d fb=%s)\n", qc.alpha_method_used, qc.m5_fallback.c_str());
      ++g_fail;
    }
    close(qc.depth, 0.5 * qc.alpha_m4, 1e-9, "после отката depth = θ·α_M4");

    // Откат: слишком мало уровней.
    const QuoteAgent qf = BuildQuoteAgent(book, m5);  // 2 уровня на сторону
    if (qf.alpha_method_used != 4 || qf.m5_fallback.find("few_levels") == std::string::npos) {
      std::printf("FAIL: few_levels-откат (used=%d fb=%s)\n", qf.alpha_method_used, qf.m5_fallback.c_str());
      ++g_fail;
    }

    // Выпуклая книга: линейная подгонка даёт отрицательный свободный член ⇒ флаг, не откат.
    std::vector<ExternalOrderLevel> cvx;
    for (int k = 1; k <= 100; ++k) {
      const double d = 200.0 * k, shift = 10.0 * (d / 20000.0) * (d / 20000.0) + 0.05;  // ‰
      const double pa = 100.0 * std::exp(shift / 1000.0), pb = 100.0 * std::exp(-shift / 1000.0);
      cvx.push_back(frac_lvl(LevelSide::kAsk, pa, 200.0 / pa));
      cvx.push_back(frac_lvl(LevelSide::kBid, pb, 200.0 / pb));
    }
    const QuoteAgent qx = BuildQuoteAgent(cvx, m5);
    if (qx.alpha_method_used != 5 || !qx.m5_negative_intercept) {
      std::printf("FAIL: выпуклая книга: used=%d neg=%d\n", qx.alpha_method_used, qx.m5_negative_intercept ? 1 : 0);
      ++g_fail;
    }

    // Робастность Хьюбера: два выброса не должны сдвигать наклон так, как МНК.
    std::vector<double> gx, gy;
    for (int j = 1; j <= 32; ++j) { gx.push_back(j / 32.0); gy.push_back(2.0 * j / 32.0); }
    gy[10] += 50.0; gy[11] += 50.0;
    double ha = 0, hb = 0;
    const bool hok = detail::HuberLine(gx, gy, &ha, &hb);
    double mx = 0, my = 0;
    for (std::size_t i = 0; i < gx.size(); ++i) { mx += gx[i]; my += gy[i]; }
    mx /= gx.size(); my /= gy.size();
    double sxx = 0, sxy = 0;
    for (std::size_t i = 0; i < gx.size(); ++i) { sxx += (gx[i] - mx) * (gx[i] - mx); sxy += (gx[i] - mx) * (gy[i] - my); }
    const double ols_b = sxy / sxx;
    if (!hok) { std::printf("FAIL: HuberLine вернул false\n"); ++g_fail; }
    close(hb, 2.0, 0.1, "Huber: наклон устойчив к выбросам");
    if (std::fabs(ols_b - 2.0) < 0.5) { std::printf("FAIL: тест выбросов не различает МНК (ols_b=%g)\n", ols_b); ++g_fail; }
    (void)ha;

    // Метод 4 по умолчанию не тронут: диагностика.
    if (a.alpha_method_used != 4 || !a.m5_fallback.empty()) {
      std::printf("FAIL: по умолчанию метод 4 без отката\n"); ++g_fail;
    }
  }

  if (g_fail == 0) { std::printf("agent_builder_test: OK\n"); return 0; }
  std::printf("agent_builder_test: %d FAIL\n", g_fail);
  return 1;
}
