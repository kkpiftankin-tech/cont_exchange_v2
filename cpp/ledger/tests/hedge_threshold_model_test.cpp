// ============================================================================
// hedge_threshold_model_test.cpp — F-18, ADR-088, CALC-CE-HEDGE-THRESHOLD-MODEL.
// Юнит-тест чистого домена hedge_threshold_model.hpp (порт ядра страницы
// incoming-docs/2026-10-29-porogi-hedzha5-380dc48d.html).
//
// Покрывает:
//   1. эталоны: порт совпадает с ядром страницы на каждом из 29 случаев (hedge_threshold_model_golden.inc,
//      генерируется tools/hedge-threshold-model/gen_golden.js);
//   2. инварианты CALC §6: порядок порогов, V'(0)≈0, законы степеней (кубический корень, ς^{-2/3}),
//      тейкер без мейкера при κ=0 и cm ≥ ct;
//   3. рабочие пороги переводчика: ограничения, режимы шума/смешанный/дрейф, непрерывность по потоку,
//      недопустимые входы;
//   4. пороги связки и перевод единиц волатильности.
// Pure unit: без PG/Kafka/env.
// ============================================================================
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

#include "domain/hedge_threshold_model.hpp"

namespace hm = cex::ledger::domain::hedge_model;

#include "hedge_threshold_model_golden.inc"

namespace {
int g_fail = 0;

void check(bool cond, const std::string& name) {
  if (cond) {
    std::cout << "[PASS] " << name << "\n";
  } else {
    std::cout << "[FAIL] " << name << "\n";
    ++g_fail;
  }
}

bool rel_near(double a, double b, double rel) {
  const double scale = std::max(1e-12, std::max(std::fabs(a), std::fabs(b)));
  return std::fabs(a - b) <= rel * scale;
}

bool abs_near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// Параметры страницы по умолчанию (главы 1–7) в единицах модели.
constexpr double kGamma = 0.0047;

hm::MtParams page_default(double fee_maker_bps = 2.0, double kappa = 1.0) {
  hm::MtParams P;
  P.A = hm::RiskA(kGamma, 4.8);
  P.sz = 2.0;
  P.theta = 0.0;
  P.kappa = kappa;
  P.cm = hm::CostMaker(fee_maker_bps, 0.5, 0.5);
  P.ct = hm::CostTaker(10.0, 0.5);
  P.h = 0.0;
  return P;
}

void test_golden_maker_taker() {
  for (const GoldenMt& g : kGoldenMt) {
    hm::MtParams P;
    P.A = g.A; P.sz = g.sz; P.theta = g.theta; P.kappa = g.kappa; P.cm = g.cm; P.ct = g.ct; P.h = 0.0;
    const hm::MtSolution s = hm::OptimizeMakerTaker(P);
    const std::string n = std::string("эталон мейкер+тейкер: ") + g.name;
    check(s.maker_zone == g.maker_zone, n + " — наличие мейкер-зоны");
    check(rel_near(s.bt, g.bt, 1e-6), n + " — порог тейкера");
    check(g.maker_zone ? rel_near(s.bm, g.bm, 1e-6) : true, n + " — порог мейкера");
    check(rel_near(s.J, g.J, 1e-6), n + " — затраты");
    check(std::fabs(s.W0) < 1e-5, n + " — V'(0) ≈ 0");
    check(s.bm >= 0.0 && s.bm <= s.bt, n + " — порядок 0 ≤ bm ≤ bt");
  }
}

void test_golden_transfer() {
  for (const GoldenTr& g : kGoldenTr) {
    hm::TrParams P;
    P.A = g.A; P.sz = g.sz; P.theta = g.theta; P.h = g.h; P.K = g.K; P.ctr = g.ctr; P.Td = g.Td;
    const hm::TrSolution s = hm::OptimizeTransfer(P);
    const std::string n = std::string("эталон перевод: ") + g.name;
    check(rel_near(s.B, g.B, 1e-6), n + " — порог B");
    check(abs_near(s.b, g.b, 1e-6 * std::max(1.0, g.B)), n + " — цель b");
    check(rel_near(s.J, g.J, 1e-6), n + " — затраты");
    check(rel_near(s.T, g.T, 1e-6), n + " — длина цикла");
  }
}

void test_golden_drift_batch_closed() {
  for (const GoldenDrift& g : kGoldenDrift) {
    hm::DriftParams P;
    P.A = g.A; P.sz = g.sz; P.mu = g.mu; P.ct = g.ct;
    const hm::DriftSolution s = hm::OptimizeDrift(P);
    const std::string n = std::string("эталон дрейф: ") + g.name;
    check(rel_near(s.b, g.b, 1e-6), n + " — полоса");
    check(rel_near(s.J, g.J, 1e-6), n + " — затраты");
    check(rel_near(s.Pe, g.Pe, 1e-6), n + " — число Пекле");
  }
  for (const GoldenBatch& g : kGoldenBatch) {
    hm::BatchParams P;
    P.A = g.A; P.mu = g.mu; P.K = g.K; P.h = g.h;
    const hm::BatchSolution s = hm::OptimizeBatch(P);
    const std::string n = std::string("эталон партия: ") + g.name;
    check(rel_near(s.H, g.H, 1e-6), n + " — размер партии");
    check(rel_near(s.J, g.J, 1e-6), n + " — затраты");
    check(rel_near(s.interval, g.interval, 1e-6), n + " — интервал");
  }
  const double A = hm::RiskA(kGamma, 4.8);
  check(rel_near(hm::CubeRootBand(hm::CostTaker(10.0, 0.5), 2.0, A), kGoldenCubeRoot, 1e-12), "формула (5): кубический корень");
  check(rel_near(hm::QuarterRootTrigger(2.0, 2.0, hm::RiskA(kGamma, 0.7)), kGoldenQuarterRoot, 1e-12), "формула (10в): корень четвёртой степени");
}

void test_units_and_calibration() {
  check(abs_near(hm::RiskA(kGamma, 4.8), 0.00054144, 1e-9), "A = 0,005·γ·ς²: 0,00054144 при γ=0,0047, ς=4,8");
  check(abs_near(hm::SigmaSecToBpsPerSqrtMin(6.13e-5), 4.7482, 1e-3), "σ за √с → bps за √мин: 6,13e-5 ⇒ 4,748");
  check(abs_near(hm::CostTaker(10.0, 0.5), 1.05, 1e-12), "c_т = 0,1·(10 + 0,5) = 1,05");
  check(abs_near(hm::CostMaker(2.0, 0.5, 0.5), 0.2, 1e-12), "c_м = 0,1·(2 − 0,5 + 0,5) = 0,2");
  check(abs_near(hm::CostMaker(10.0, 0.5, 0.5), 1.0, 1e-12), "c_м при стандартной комиссии 10 bps = 1,0");
  check(abs_near(hm::CapitalCostPerMin(10.0), 1000.0 * 10.0 / 100.0 / 525600.0, 1e-15), "h = 10 % годовых в USDT/мин на 1 тыс. USDT");
  // калибровка γ: при f_т = 10 bps полоса только-тейкера ≈ 18 тыс. USDT (страница, глава 4)
  hm::MtParams P = page_default();
  P.kappa = 0.0;
  P.cm = P.ct;
  const hm::MtSolution s = hm::OptimizeMakerTaker(P);
  check(abs_near(s.bt, 17.9854, 1e-3), "калибровка: только тейкер b_т ≈ 17,985");
  check(rel_near(s.taker / s.J, 2.0 / 3.0, 1e-3), "на оптимуме комиссии ≈ ⅔ затрат (формула 5а)");
}

void test_power_laws() {
  hm::MtParams P = page_default();
  P.kappa = 0.0;
  P.cm = P.ct;
  const double b1 = hm::OptimizeMakerTaker(P).bt;
  hm::MtParams P2 = P;
  P2.ct *= 2.0;
  P2.cm = P2.ct;
  check(rel_near(hm::OptimizeMakerTaker(P2).bt / b1, std::cbrt(2.0), 1e-2), "закон степени: c_т ×2 ⇒ b_т ×2^{1/3}");
  hm::MtParams P3 = P;
  P3.A *= 4.0;  // ς ×2 ⇒ A ×4
  check(rel_near(hm::OptimizeMakerTaker(P3).bt / b1, std::pow(4.0, -1.0 / 3.0), 1e-2), "закон степени: ς ×2 (A ×4) ⇒ b_т ×2^{-2/3}");
  hm::MtParams P4 = P;
  P4.sz *= 2.0;
  check(rel_near(hm::OptimizeMakerTaker(P4).bt / b1, std::pow(2.0, 2.0 / 3.0), 1e-2), "закон степени: ς_z ×2 ⇒ b_т ×2^{2/3}");
  // мейкер не нужен, если он не дешевле тейкера: решение совпадает с чистым тейкером
  hm::MtParams P5 = page_default(20.0, 1.0);
  const hm::MtSolution s5 = hm::OptimizeMakerTaker(P5);
  check(!s5.maker_zone && rel_near(s5.bt, b1, 1e-6), "c_м ≥ c_т ⇒ мейкер-зоны нет, b_т = полоса только тейкера");
  // монотонность по скорости мейкера: быстрее мейкер ⇒ порог тейкера дальше
  const double bt_slow = hm::OptimizeMakerTaker(page_default(2.0, 0.05)).bt;
  const double bt_mid = hm::OptimizeMakerTaker(page_default(2.0, 1.0)).bt;
  const double bt_fast = hm::OptimizeMakerTaker(page_default(2.0, 20.0)).bt;
  check(bt_slow < bt_mid && bt_mid < bt_fast, "κ растёт ⇒ b_т растёт");
}

void test_translator_band() {
  hm::ModelSettings S;  // значения страницы
  hm::TranslatorInputs in;
  in.sigma_bps = 4.8; in.fee_taker_bps = 10.0; in.fee_maker_bps = 2.0; in.sz = 2.0; in.mu = 0.0;
  const hm::TranslatorBand b = hm::ComputeTranslatorBand(S, in, 2.0, 1000.0);
  check(b.ok && b.regime == hm::Regime::kNoise, "переводчик: допустимые входы, режим шума");
  check(abs_near(b.z_lim, 8.373, 5e-3) && abs_near(b.z_mkt, 41.006, 5e-3), "переводчик: ставки 2/10 ⇒ пороги 8,373 и 41,006");
  check(b.maker_zone, "переводчик: мейкер-зона есть");
  in.fee_maker_bps = 10.0;
  const hm::TranslatorBand bs = hm::ComputeTranslatorBand(S, in, 2.0, 1000.0);
  check(abs_near(bs.z_lim, 15.971, 5e-3) && abs_near(bs.z_mkt, 20.215, 5e-3), "переводчик: стандартная комиссия 10/10 ⇒ пороги 15,971 и 20,215");
  // ограничения: пол и верхняя граница
  const hm::TranslatorBand fl = hm::ComputeTranslatorBand(S, in, 30.0, 1000.0);
  check(fl.ok && fl.z_lim == 30.0 && fl.z_mkt >= fl.z_lim, "переводчик: пол порога поднимает z_lim, z_mkt ≥ z_lim");
  const hm::TranslatorBand cp = hm::ComputeTranslatorBand(S, in, 2.0, 12.0);
  check(cp.ok && cp.z_mkt == 12.0 && cp.z_lim <= cp.z_mkt, "переводчик: верхняя граница обрезает z_mkt");
  // недопустимые входы
  hm::TranslatorInputs bad = in;
  bad.sigma_bps = 0.0;
  check(!hm::ComputeTranslatorBand(S, bad, 2.0, 1000.0).ok, "переводчик: нулевая волатильность ⇒ модель не считается");
  bad = in; bad.sz = -1.0;
  check(!hm::ComputeTranslatorBand(S, bad, 2.0, 1000.0).ok, "переводчик: отрицательный шум ⇒ не считается");
  bad = in; bad.fee_taker_bps = std::nan("");
  check(!hm::ComputeTranslatorBand(S, bad, 2.0, 1000.0).ok, "переводчик: NaN комиссия ⇒ не считается");
  hm::ModelSettings S0 = S;
  S0.gamma = 0.0;
  check(!hm::ComputeTranslatorBand(S0, in, 2.0, 1000.0).ok, "переводчик: γ = 0 ⇒ не считается");
}

void test_regimes_and_continuity() {
  hm::ModelSettings S;
  hm::TranslatorInputs in;
  in.sigma_bps = 4.8; in.fee_taker_bps = 10.0; in.fee_maker_bps = 2.0; in.sz = 2.0;
  double prev_mkt = 1e9, prev_lim = 1e9;
  bool monotone = true, ordered = true, saw_mixed = false, saw_drift = false;
  for (int i = 0; i <= 400; ++i) {
    in.mu = 0.0 + 0.0125 * i;  // 0 … 5 тыс. USDT/мин
    const hm::TranslatorBand b = hm::ComputeTranslatorBand(S, in, 2.0, 1000.0);
    if (!b.ok) { monotone = false; break; }
    if (i > 0 && (b.z_mkt > prev_mkt + 1e-9 || b.z_lim > prev_lim + 1e-9)) monotone = false;
    if (b.z_lim > b.z_mkt + 1e-12 || b.z_lim < 2.0 - 1e-12) ordered = false;
    prev_mkt = b.z_mkt; prev_lim = b.z_lim;
    if (b.regime == hm::Regime::kMixed) saw_mixed = true;
    if (b.regime == hm::Regime::kDrift) saw_drift = true;
  }
  check(monotone, "дрейф: оба порога не растут с потоком");
  check(ordered, "дрейф: 2 ≤ z_lim ≤ z_mkt при любом потоке");
  check(saw_mixed && saw_drift, "дрейф: встретились смешанный режим и дрейф");
  // непрерывность на границах режимов Pe = 0,3 и Pe = 3 (mu = Pe·ς_z²/(2·b_т))
  {
    in.mu = 0.0;
    const double bt = hm::ComputeTranslatorBand(S, in, 2.0, 1000.0).b_t_noise;
    for (double pe : {S.pe_noise, S.pe_drift}) {
      const double mu0 = pe * in.sz * in.sz / (2.0 * bt);
      hm::TranslatorInputs lo = in, hi = in;
      lo.mu = mu0 * (1.0 - 1e-6);
      hi.mu = mu0 * (1.0 + 1e-6);
      const hm::TranslatorBand a = hm::ComputeTranslatorBand(S, lo, 2.0, 1000.0);
      const hm::TranslatorBand c = hm::ComputeTranslatorBand(S, hi, 2.0, 1000.0);
      check(std::fabs(a.z_mkt - c.z_mkt) < 0.01 && std::fabs(a.z_lim - c.z_lim) < 0.01,
            std::string("дрейф: порог непрерывен на границе режимов Pe = ") + std::to_string(pe));
    }
  }
  // мейкер-зона в дрейфе схлопывается при сильном потоке
  in.mu = 5.0;
  const hm::TranslatorBand deep = hm::ComputeTranslatorBand(S, in, 0.0, 1000.0);
  check(deep.regime == hm::Regime::kDrift && !deep.maker_zone && deep.z_lim == deep.z_mkt, "дрейф mu=5: мейкер-зона схлопнута, z_lim = z_mkt");
  // нулевой поток и режим шума — чистое решение
  in.mu = 0.0;
  const hm::TranslatorBand z = hm::ComputeTranslatorBand(S, in, 2.0, 1000.0);
  check(z.regime == hm::Regime::kNoise && z.blend == 0.0, "поток 0 ⇒ режим шума, вес дрейфа 0");
  // сильный поток: полоса дрейфа уже шумовой и близка к странице (mu=10 ⇒ b ≈ 1,22; пол 2 поднимает)
  in.mu = 10.0;
  const hm::TranslatorBand dr = hm::ComputeTranslatorBand(S, in, 0.0, 1000.0);
  check(dr.regime == hm::Regime::kDrift && abs_near(dr.z_mkt, 1.2159, 5e-3), "дрейф mu=10: полоса ≈ 1,216 (эталон страницы)");
}

void test_link_band() {
  hm::ModelSettings S;
  hm::LinkInputs in;
  in.sigma_basis_bps = 0.7; in.sz = 2.0; in.K = 2.0; in.ctr_bps = 0.0; in.capital_pct = 10.0; in.transfer_min = 0.0;
  const hm::LinkBand l = hm::ComputeLinkBand(S, in, 1000.0);
  check(l.ok && abs_near(l.B, 41.557, 5e-3) && abs_near(l.b, 0.0, 1e-6), "связка: K=2 USDT ⇒ B ≈ 41,557, цель 0");
  check(abs_near(l.perHour, 0.139, 5e-3), "связка: 0,139 перевода в час");
  const hm::LinkBand capped = hm::ComputeLinkBand(S, in, 10.0);
  check(capped.ok && capped.B == 10.0 && capped.b <= capped.B, "связка: верхняя граница обрезает порог, цель не выше порога");
  hm::LinkInputs bad = in;
  bad.sigma_basis_bps = 0.0;
  check(!hm::ComputeLinkBand(S, bad, 1000.0).ok, "связка: нулевая волатильность базиса ⇒ не считается");
  bad = in; bad.K = 0.0; bad.ctr_bps = 0.0;
  check(!hm::ComputeLinkBand(S, bad, 1000.0).ok, "связка: нет ни фиксированной платы, ни процента ⇒ не считается");
  hm::LinkInputs pct = in;
  pct.ctr_bps = 2.5;
  const hm::LinkBand lp = hm::ComputeLinkBand(S, pct, 1000.0);
  check(lp.ok && lp.b > 1.0 && lp.b < lp.B, "связка: с процентом от суммы цель внутри (0 < b < B)");
  // Ревью D4: бесконечный/нечисловой вход не доходит до приведения к int в расчёте затрат (UB) — связка не считается.
  const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
  bad = in; bad.sz = inf;
  check(!hm::ComputeLinkBand(S, bad, 1000.0).ok, "связка: бесконечный шум ⇒ не считается");
  bad = in; bad.sigma_basis_bps = nan;
  check(!hm::ComputeLinkBand(S, bad, 1000.0).ok, "связка: NaN волатильности базиса ⇒ не считается");
  bad = in; bad.capital_pct = inf;
  check(!hm::ComputeLinkBand(S, bad, 1000.0).ok, "связка: бесконечная стоимость капитала ⇒ не считается");
  hm::TrParams tp;
  tp.A = 0.01; tp.sz = 1.0; tp.K = 2.0;
  const hm::TrCost nc = hm::TrCostAt(nan, 10.0, tp);
  check(!std::isfinite(nc.J) || nc.J > 1e300, "затраты цикла перевода при NaN-цели: пустой цикл, без падения (J не конечна)");
}
}  // namespace

int main() {
  test_units_and_calibration();
  test_golden_maker_taker();
  test_golden_transfer();
  test_golden_drift_batch_closed();
  test_power_laws();
  test_translator_band();
  test_regimes_and_continuity();
  test_link_band();
  std::cout << (g_fail == 0 ? "ALL PASS\n" : "FAILURES: ") << (g_fail == 0 ? "" : std::to_string(g_fail) + "\n");
  return g_fail == 0 ? 0 : 1;
}
