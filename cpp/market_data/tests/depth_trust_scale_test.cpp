// ADR-085: уценка устаревших котировок и капитальный предел глубины агента. Чистые функции.
// Числа взяты со страницы владельца («Интерактивный клиринг CE v6», спецификация интеграции, патчи 1–2) и из примера базового документа
// «Кривые котирования» (ур. 6.2: θ = 0,6, α_ext = 83,3, предел 60 ⇒ α_T = 50).
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

#include "domain/depth_trust_scale.hpp"

namespace {

using cex::market_data::domain::AgeTrustWeight;
using cex::market_data::domain::CapitalDepthCap;
using cex::market_data::domain::ComposeDepth;
using cex::market_data::domain::TrustDropThresholdMs;
using cex::market_data::domain::ParseTrustMode;
using cex::market_data::domain::TrustExcluded;
using cex::market_data::domain::TrustMode;
using cex::market_data::domain::TrustScaleConfig;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}
bool Near(const double a, const double b, const double eps = 1e-9) { return std::fabs(a - b) <= eps; }

TrustScaleConfig Cfg(const TrustMode m, const double psi_min = 0.0) {
  TrustScaleConfig c;
  c.mode = m;
  c.psi_min = psi_min;   // 0 ⇒ вес = сырой ψ (числа страницы без порога)
  return c;
}

bool TestParseMode() {
  return Check(ParseTrustMode("LINEAR") == TrustMode::kLinear && ParseTrustMode("1") == TrustMode::kLinear, "LINEAR parses") &&
         Check(ParseTrustMode("DISP") == TrustMode::kDisp && ParseTrustMode("disp") == TrustMode::kDisp, "DISP parses") &&
         Check(ParseTrustMode("") == TrustMode::kOff && ParseTrustMode("OFF") == TrustMode::kOff && ParseTrustMode("0") == TrustMode::kOff &&
                   ParseTrustMode("мусор") == TrustMode::kOff,
               "anything else is off");
}

// Страница, патч 1: возраст 190 мс, T_stale 500 мс, LINEAR ⇒ ψ = 0,620; глубина 50 ⇒ 31,0.
bool TestLinearPageExample() {
  const double psi = AgeTrustWeight(Cfg(TrustMode::kLinear), 0.19, 0.5, 0.0);
  return Check(Near(psi, 0.62), "linear psi = 1 - 0.19/0.5 = 0.62") && Check(Near(50.0 * psi, 31.0), "depth 50 -> 31.0");
}

// Страница, патч 1: возраст 190 мс, σ0 = 0,5‰, ς = 0,79‰/√с ⇒ T_ς = 0,4006 с, ψ = 0,678; глубина 50 ⇒ 33,9.
bool TestDispPageExample() {
  TrustScaleConfig c = Cfg(TrustMode::kDisp);
  c.sig0_pm = 0.5;
  const double psi = AgeTrustWeight(c, 0.19, 0.0, 0.79);
  const double t_sigma = (0.5 / 0.79) * (0.5 / 0.79);
  return Check(Near(psi, 1.0 / (1.0 + 0.19 / t_sigma), 1e-12), "disp psi formula") && Check(std::fabs(psi - 0.678) < 5e-4, "disp psi ~ 0.678") &&
         Check(std::fabs(50.0 * psi - 33.9) < 0.05, "depth 50 -> 33.9");
}

// Нулевой возраст ⇒ 1; возраст на пределе ⇒ 0; за пределом ⇒ 0 (не отрицательный).
bool TestLinearBounds() {
  const TrustScaleConfig c = Cfg(TrustMode::kLinear);
  return Check(AgeTrustWeight(c, 0.0, 60.0, 0.0) == 1.0, "age 0 -> 1") && Check(AgeTrustWeight(c, -5.0, 60.0, 0.0) == 1.0, "negative age -> 1") &&
         Check(Near(AgeTrustWeight(c, 60.0, 60.0, 0.0), 0.0), "age = T_stale -> 0") && Check(AgeTrustWeight(c, 300.0, 60.0, 0.0) == 0.0, "beyond T_stale -> 0") &&
         Check(Near(AgeTrustWeight(c, 30.0, 60.0, 0.0), 0.5), "half way -> 0.5");
}

// Нулевая волатильность ⇒ верим всегда; растёт волатильность ⇒ вес падает; растёт возраст ⇒ вес падает.
bool TestDispMonotoneAndCalm() {
  const TrustScaleConfig c = Cfg(TrustMode::kDisp);
  return Check(AgeTrustWeight(c, 100.0, 0.0, 0.0) == 1.0, "zero volatility -> always trust") &&
         Check(AgeTrustWeight(c, 5.0, 0.0, 1.0) < AgeTrustWeight(c, 5.0, 0.0, 0.5), "higher volatility lowers the weight") &&
         Check(AgeTrustWeight(c, 10.0, 0.0, 0.5) < AgeTrustWeight(c, 2.0, 0.0, 0.5), "older snapshot lowers the weight") &&
         Check(Near(AgeTrustWeight(c, 1.0, 0.0, 0.5), 0.5), "age = T_sigma = (0.5/0.5)^2 = 1 s -> 0.5");
}

// Выключено или нечисловой вход ⇒ ничего не режем.
bool TestNeutralInputs() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  return Check(AgeTrustWeight(Cfg(TrustMode::kOff), 100.0, 60.0, 1.0) == 1.0, "off -> 1") &&
         Check(AgeTrustWeight(Cfg(TrustMode::kLinear), nan, 60.0, 0.0) == 1.0, "NaN age -> 1") &&
         Check(AgeTrustWeight(Cfg(TrustMode::kLinear), 5.0, nan, 0.0) == 1.0, "NaN T_stale -> 1") &&
         Check(AgeTrustWeight(Cfg(TrustMode::kLinear), 5.0, 0.0, 0.0) == 1.0, "T_stale 0 -> 1") &&
         Check(AgeTrustWeight(Cfg(TrustMode::kDisp), 5.0, 0.0, nan) == 1.0, "NaN volatility -> 1") &&
         Check(AgeTrustWeight(Cfg(TrustMode::kDisp), inf, 0.0, 1.0) == 1.0, "inf age -> 1");
}

// Порог ψ_min: итоговый вес w = (ψ − ψ_min)/(1 − ψ_min), гладко до нуля; w = 0 ⇒ исключение; выключено ⇒ никогда.
bool TestHingeAndExclusion() {
  const TrustScaleConfig c = Cfg(TrustMode::kLinear, 0.05);
  const double w_mid = AgeTrustWeight(c, 0.19, 0.5, 0.0);          // ψ = 0,62 ⇒ (0,62 − 0,05)/0,95 = 0,6
  const double w_edge = AgeTrustWeight(c, 0.475, 0.5, 0.0);        // ψ = 0,05 ⇒ 0
  const double w_beyond = AgeTrustWeight(c, 0.49, 0.5, 0.0);       // ψ = 0,02 ⇒ 0 (не отрицательный)
  return Check(Near(w_mid, 0.6), "hinge: psi 0.62 -> (0.62-0.05)/0.95 = 0.6") && Check(Near(w_edge, 0.0), "psi = psi_min -> ~0") && Check(TrustExcluded(c, w_edge), "boundary weight (float dust) is excluded too") &&
         Check(w_beyond == 0.0, "psi below psi_min -> 0") && Check(TrustExcluded(c, w_beyond), "zero weight -> excluded") &&
         Check(!TrustExcluded(c, w_mid), "positive weight -> kept") && Check(!TrustExcluded(Cfg(TrustMode::kOff), 0.0), "off never excludes") &&
         Check(AgeTrustWeight(c, 0.0, 0.5, 0.0) == 1.0, "fresh snapshot keeps weight 1 under hinge");
}

// DISP: хвост 1/Δt нуля не достигает, поэтому без порога котировка не исключается никогда, а с порогом ψ_min уходит гладко.
bool TestDispTailNeedsHinge() {
  TrustScaleConfig c = Cfg(TrustMode::kDisp, 0.0);
  c.sig0_pm = 0.5;
  const double w_old = AgeTrustWeight(c, 1e6, 0.0, 1.0);
  TrustScaleConfig h = c;
  h.psi_min = 0.05;
  const double w_old_h = AgeTrustWeight(h, 1e6, 0.0, 1.0);
  return Check(w_old > 0.0 && !TrustExcluded(c, w_old), "disp without hinge never reaches zero") &&
         Check(w_old_h == 0.0 && TrustExcluded(h, w_old_h), "disp with hinge goes to zero and is excluded");
}

// Проверка специалиста: σ0 = 0,3‰, ς = 0,09‰/√с ⇒ T_ς = 11,1 с; Δt = 5 с ⇒ ψ = 0,69. Волатильность ×2 ⇒ T_ς = 2,8 с, ψ = 0,36.
bool TestDispSpecialistExample() {
  TrustScaleConfig c = Cfg(TrustMode::kDisp, 0.0);
  c.sig0_pm = 0.3;
  return Check(std::fabs(AgeTrustWeight(c, 5.0, 0.0, 0.09) - 0.69) < 5e-3, "psi ~ 0.69 at dt = 5 s") &&
         Check(std::fabs(AgeTrustWeight(c, 5.0, 0.0, 0.18) - 0.357) < 5e-3, "volatility x2 -> psi ~ 0.36");
}

// LINEAR с пределом из адаптивного порога репозитория: T = 9,24 с ⇒ при Δt = 3 с ψ = 0,675; при σ×2 T = 2,31 с ⇒ Δt = 3 с даёт 0 (исключение).
bool TestLinearAdaptiveLimitArithmetic() {
  const TrustScaleConfig c = Cfg(TrustMode::kLinear, 0.0);
  return Check(std::fabs(AgeTrustWeight(c, 3.0, 9.24, 0.0) - 0.675) < 1e-3, "psi ~ 0.675 with T = 9.24 s") &&
         Check(AgeTrustWeight(c, 3.0, 2.31, 0.0) == 0.0, "age beyond T = 2.31 s -> 0");
}

// Страница, патч 2: γς² = 0,4 ‰/с (γ = 0,1 1/‰, ς = 2 ‰/√с), τ = 0,5 с ⇒ риск 0,2‰; W_T = 10 ⇒ 50; τ = 1 ⇒ 25.
bool TestCapitalCapPageExample() {
  return Check(Near(CapitalDepthCap(10.0, 0.1, 2.0, 0.5), 50.0, 1e-9), "cap = 10 / (0.1*4*0.5) = 50") &&
         Check(Near(CapitalDepthCap(10.0, 0.1, 2.0, 1.0), 25.0, 1e-9), "tau 1 s -> 25") &&
         Check(Near(CapitalDepthCap(10.0, 0.1, 4.0, 0.5), 12.5, 1e-9), "vol x2 -> cap / 4");
}

// Базовый документ, ур. (6.2): θ = 0,6, α_ext = 83,3 ⇒ θα = 50; капитальный предел 60 ⇒ α_T = 50; предел 40 ⇒ 40.
bool TestComposeDepthBaseDocExample() {
  const double theta_alpha = 0.6 * (250.0 / 3.0);  // минорант min{100/1; 250/3; 500/6} = 83,33
  const auto a = ComposeDepth(theta_alpha, 60.0, 1.0, 1.0);
  const auto b = ComposeDepth(theta_alpha, 40.0, 1.0, 1.0);
  return Check(Near(a.depth, 50.0, 1e-9) && !a.capped, "cap 60 does not bind: alpha_T = 50") &&
         Check(Near(b.depth, 40.0, 1e-9) && b.capped, "cap 40 binds: alpha_T = 40");
}

// Порядок «предел первым, затем f и вес»: θα = 50, w = 0,69, f = 0,9, cap = 25 ⇒ 0,621·25 = 15,525. «Предел последним» дал бы 25 без действия f и w.
bool TestComposeDepthOrder() {
  const auto r = ComposeDepth(50.0, 25.0, 0.9, 0.69);
  const double min_last = std::min(50.0 * 0.9 * 0.69, 25.0);
  return Check(Near(r.depth, 25.0 * 0.9 * 0.69, 1e-9) && r.capped, "cap first, then fill and trust weight (15.525)") &&
         Check(Near(min_last, 25.0, 1e-9) && r.depth < min_last, "min-last would ignore weight and fill on a binding cap") &&
         Check(r.depth < ComposeDepth(50.0, 25.0, 0.9, 0.9).depth, "older snapshot lowers depth even when cap binds");
}

// Выключено (множители 1, предела нет) ⇒ глубина совпадает бит в бит.
bool TestComposeDepthNeutral() {
  const double inf = std::numeric_limits<double>::infinity();
  const double d = 12.345678901234;
  const auto r = ComposeDepth(d, inf, 1.0, 1.0);
  return Check(r.depth == d && !r.capped, "no cap and unit multipliers keep depth bit-exact");
}

// Порог, на котором вес становится нулём: LINEAR T·(1 − ψ_min); DISP T_ς·(1/ψ_min − 1).
bool TestDropThreshold() {
  TrustScaleConfig lin = Cfg(TrustMode::kLinear, 0.05);
  TrustScaleConfig dsp = Cfg(TrustMode::kDisp, 0.05);
  dsp.sig0_pm = 0.3;
  return Check(Near(TrustDropThresholdMs(lin, 60.0, 0.0), 57000.0, 1e-6), "linear: 60 s * 0.95 = 57 s") &&
         Check(Near(TrustDropThresholdMs(dsp, 0.0, 0.09), 1000.0 * (0.3 / 0.09) * (0.3 / 0.09) * 19.0, 1e-6), "disp: T_sigma * (1/0.05 - 1)") &&
         Check(Near(TrustDropThresholdMs(dsp, 60.0, 0.0), 57000.0, 1e-6), "disp without volatility falls back to T_stale");
}

// Крайние входы: ψ_min ≥ 1 обрезан до 0,99; NaN в весе не исключает; σ0 ≤ 0 в DISP ⇒ верим.
bool TestEdgeInputs() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  TrustScaleConfig c = Cfg(TrustMode::kLinear, 1.0);   // ψ_min = 1 ⇒ обрезается до 0,99, деления на ноль нет
  const double w = AgeTrustWeight(c, 0.1, 1.0, 0.0);   // ψ = 0,9 < 0,99 ⇒ 0
  TrustScaleConfig d = Cfg(TrustMode::kDisp, 0.0);
  d.sig0_pm = 0.0;
  return Check(std::isfinite(w) && w == 0.0, "psi_min >= 1 is clamped, no division by zero") && Check(!TrustExcluded(Cfg(TrustMode::kLinear), nan), "NaN weight is not excluded") &&
         Check(AgeTrustWeight(d, 5.0, 0.0, 0.5) == 1.0, "DISP with sigma0 <= 0 trusts the snapshot");
}

// Нет данных ⇒ предела нет.
bool TestCapitalCapDisabled() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  return Check(CapitalDepthCap(0.0, 0.1, 2.0, 0.5) == inf, "W_T = 0 -> no cap") && Check(CapitalDepthCap(10.0, 0.0, 2.0, 0.5) == inf, "gamma = 0 -> no cap") &&
         Check(CapitalDepthCap(10.0, 0.1, 0.0, 0.5) == inf, "sigma = 0 -> no cap") && Check(CapitalDepthCap(10.0, 0.1, 2.0, 0.0) == inf, "tau = 0 -> no cap") &&
         Check(CapitalDepthCap(nan, 0.1, 2.0, 0.5) == inf, "NaN -> no cap") && Check(CapitalDepthCap(10.0, 0.1, -2.0, 0.5) == inf, "negative sigma -> no cap");
}

}  // namespace

int main() {
  const bool ok = TestParseMode() && TestLinearPageExample() && TestDispPageExample() && TestLinearBounds() && TestDispMonotoneAndCalm() &&
                  TestNeutralInputs() && TestHingeAndExclusion() && TestDispTailNeedsHinge() && TestDispSpecialistExample() &&
                  TestLinearAdaptiveLimitArithmetic() && TestCapitalCapPageExample() && TestComposeDepthBaseDocExample() && TestComposeDepthOrder() && TestComposeDepthNeutral() && TestDropThreshold() && TestEdgeInputs() && TestCapitalCapDisabled();
  if (!ok) return 1;
  std::cout << "all passed\n";
  return 0;
}
