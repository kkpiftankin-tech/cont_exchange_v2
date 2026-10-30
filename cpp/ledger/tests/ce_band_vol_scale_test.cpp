// ============================================================================
// ce_band_vol_scale_test.cpp — F-18 D1+#7 (ADR-066, T-F18-703/706). Unit-тесты
// чистой функции LedgerUseCases::VolScaleFactor — множителя динамического порога
// band по волатильности: scale = clamp((σ_ref²/σ²)/γ, vr_min, vr_max).
//
// Покрывает регрессию domain-review HIGH: клэмпится ИТОГОВЫЙ scale (не только
// ratio), т.е. γ-канал (общее поле capital-cap) не выводит порог за диапазон.
// Проверяет: анкер при σ=σ_ref,γ=1 → 1.0 (⇒ flat-порог не меняется); знак
// (σ↑ ⇒ scale↓, полоса уже); клэмп на обеих границах; γ<1 и γ→0; σ≤0 → 1.0.
//
// Pure unit (без PG/Kafka): VolScaleFactor — static, зависит только от аргументов.
// ============================================================================
#include <cmath>
#include <iostream>
#include <string>

#include "app/ledger_uc.hpp"

using cex::ledger::app::LedgerUseCases;

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

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

// Границы клэмпа как в detect (env-дефолты CE_BAND_VOL_RATIO_MIN/MAX).
constexpr double kMin = 0.1;
constexpr double kMax = 10.0;
constexpr double kRef = 0.0005;  // CE_BAND_SIGMA_REF по умолчанию
}  // namespace

int main() {
  // 1. Анкер: σ=σ_ref, γ=1 ⇒ scale=1 (динамика не трогает плоский порог).
  check(near(LedgerUseCases::VolScaleFactor(kRef, kRef, 1.0, kMin, kMax), 1.0),
        "anchor: sigma=sigma_ref, gamma=1 -> 1.0");

  // 2. σ вдвое выше σ_ref ⇒ ratio=0.25 ⇒ полоса уже (scale<1).
  check(near(LedgerUseCases::VolScaleFactor(2 * kRef, kRef, 1.0, kMin, kMax), 0.25),
        "higher sigma -> tighter (0.25)");

  // 3. σ намного выше ⇒ ratio→мал ⇒ клэмп снизу 0.1.
  check(near(LedgerUseCases::VolScaleFactor(10 * kRef, kRef, 1.0, kMin, kMax), kMin),
        "very high sigma -> clamp low 0.1");

  // 4. σ намного ниже ⇒ ratio→велик ⇒ клэмп сверху 10.
  check(near(LedgerUseCases::VolScaleFactor(0.1 * kRef, kRef, 1.0, kMin, kMax), kMax),
        "very low sigma -> clamp high 10");

  // 5. γ=0.5 при σ=σ_ref ⇒ scale=1/0.5=2 (в диапазоне).
  check(near(LedgerUseCases::VolScaleFactor(kRef, kRef, 0.5, kMin, kMax), 2.0),
        "gamma=0.5 -> 2.0 (within range)");

  // 6. РЕГРЕССИЯ HIGH: γ=0.05 ⇒ scale=1/0.05=20 ⇒ клэмп 10 (не 20!).
  check(near(LedgerUseCases::VolScaleFactor(kRef, kRef, 0.05, kMin, kMax), kMax),
        "REGRESSION: small gamma clamped to 10 (not 20)");

  // 7. γ→0 ⇒ guard max(1e-6) ⇒ scale огромен ⇒ клэмп 10 (нет overflow/inf).
  check(near(LedgerUseCases::VolScaleFactor(kRef, kRef, 1e-12, kMin, kMax), kMax),
        "gamma->0 guarded and clamped to 10");

  // 8. σ<=0 ⇒ 1.0 (нет данных — без масштаба; caller и так не вызывает, но защищаемся).
  check(near(LedgerUseCases::VolScaleFactor(0.0, kRef, 1.0, kMin, kMax), 1.0),
        "sigma=0 -> 1.0 (no scale)");
  check(near(LedgerUseCases::VolScaleFactor(-1.0, kRef, 1.0, kMin, kMax), 1.0),
        "sigma<0 -> 1.0 (no scale)");

  // 9. σ_ref<=0 ⇒ 1.0 (некалибровано).
  check(near(LedgerUseCases::VolScaleFactor(kRef, 0.0, 1.0, kMin, kMax), 1.0),
        "sigma_ref=0 -> 1.0 (uncalibrated)");

  // 10. Монотонность: чем выше σ, тем меньше scale (при прочих равных).
  const double s_lo = LedgerUseCases::VolScaleFactor(0.8 * kRef, kRef, 1.0, kMin, kMax);
  const double s_hi = LedgerUseCases::VolScaleFactor(1.2 * kRef, kRef, 1.0, kMin, kMax);
  check(s_lo > s_hi, "monotonic: lower sigma -> wider band than higher sigma");

  if (g_fail == 0) {
    std::cout << "ALL PASS (ce_band_vol_scale_test)\n";
    return 0;
  }
  std::cout << g_fail << " FAILED\n";
  return 1;
}
