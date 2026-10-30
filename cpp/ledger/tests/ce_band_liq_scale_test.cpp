// ============================================================================
// ce_band_liq_scale_test.cpp — F-18 #7 scoped (ADR-066 §D3, T-F18-L03/L04).
// Unit-тесты чистой функции LedgerUseCases::LiqScaleFactor — ликвидностного
// множителя порога band: LiqScale = clamp((depth/depth_ref)^p, lr_min, lr_max),
// p=0.5. Направление ПРЯМОЕ (в отличие от VolScaleFactor): глубже стакан ⇒ ШИРЕ
// band (по IN-017 Q∝√Λ).
//
// Инвариант LIQ-1: LiqScale ∈ [lr_min, lr_max], монотонно не убывает по depth.
// depth≤0 / depth_ref≤0 ⇒ 1.0 (нейтрально, LIQ-4 fallback на стороне caller).
//
// Pure unit (без PG/Kafka): LiqScaleFactor — static.
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

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// Границы клэмпа как env-дефолты CE_BAND_LIQ_RATIO_MIN/MAX.
constexpr double kMin = 0.5;
constexpr double kMax = 2.0;
constexpr double kRef = 1.0;  // depth_ref (тыс.USDT/‰)
constexpr double kP = 0.5;
}  // namespace

int main() {
  // 1. Анкер: depth=depth_ref ⇒ scale=1.0 (порог не трогается).
  check(near(LedgerUseCases::LiqScaleFactor(kRef, kRef, kP, kMin, kMax), 1.0),
        "anchor: depth=depth_ref -> 1.0");

  // 2. depth вдвое глубже ⇒ √2≈1.414 (ШИРЕ band, в диапазоне).
  check(near(LedgerUseCases::LiqScaleFactor(2 * kRef, kRef, kP, kMin, kMax), std::sqrt(2.0)),
        "2x depth -> sqrt(2)~1.414 (wider)");

  // 3. depth=4x ⇒ √4=2.0 (ровно на верхней границе).
  check(near(LedgerUseCases::LiqScaleFactor(4 * kRef, kRef, kP, kMin, kMax), 2.0),
        "4x depth -> 2.0 (at clamp high)");

  // 4. depth=9x ⇒ 3.0 ⇒ клэмп сверху 2.0 (не 3.0!).
  check(near(LedgerUseCases::LiqScaleFactor(9 * kRef, kRef, kP, kMin, kMax), kMax),
        "9x depth -> clamp high 2.0 (not 3.0)");

  // 5. depth=0.25x ⇒ √0.25=0.5 (ровно на нижней границе — тонкий рынок, у́же band).
  check(near(LedgerUseCases::LiqScaleFactor(0.25 * kRef, kRef, kP, kMin, kMax), 0.5),
        "0.25x depth -> 0.5 (at clamp low, tighter)");

  // 6. depth=0.01x ⇒ 0.1 ⇒ клэмп снизу 0.5.
  check(near(LedgerUseCases::LiqScaleFactor(0.01 * kRef, kRef, kP, kMin, kMax), kMin),
        "0.01x depth -> clamp low 0.5");

  // 7. depth<=0 ⇒ 1.0 (нет глубины — без масштаба).
  check(near(LedgerUseCases::LiqScaleFactor(0.0, kRef, kP, kMin, kMax), 1.0),
        "depth=0 -> 1.0 (no scale)");
  check(near(LedgerUseCases::LiqScaleFactor(-1.0, kRef, kP, kMin, kMax), 1.0),
        "depth<0 -> 1.0 (no scale)");

  // 8. depth_ref<=0 ⇒ 1.0 (некалибровано).
  check(near(LedgerUseCases::LiqScaleFactor(2 * kRef, 0.0, kP, kMin, kMax), 1.0),
        "depth_ref=0 -> 1.0 (uncalibrated)");

  // 9. Монотонность (ПРЯМАЯ): глубже ⇒ scale больше (противоположно VolScaleFactor).
  const double d_lo = LedgerUseCases::LiqScaleFactor(0.8 * kRef, kRef, kP, kMin, kMax);
  const double d_hi = LedgerUseCases::LiqScaleFactor(1.5 * kRef, kRef, kP, kMin, kMax);
  check(d_hi > d_lo, "monotonic: deeper -> wider band than shallower");

  // 10. Направление: глубже ref ⇒ scale>1 (шире); мельче ⇒ scale<1 (уже).
  check(LedgerUseCases::LiqScaleFactor(1.5 * kRef, kRef, kP, kMin, kMax) > 1.0 &&
        LedgerUseCases::LiqScaleFactor(0.6 * kRef, kRef, kP, kMin, kMax) < 1.0,
        "direction: deep>1 (wider), shallow<1 (tighter)");

  // ---- BandTotalScale: композиция двух осей + финальный клэмп (LIQ-2/LIQ-3) ----
  constexpr double kTrMin = 0.1;   // CE_BAND_TOTAL_RATIO_MIN дефолт
  constexpr double kTrMax = 15.0;  // CE_BAND_TOTAL_RATIO_MAX дефолт

  // LIQ-3: обе оси нейтральны (1.0) ⇒ total=1.0 (поведение как до фичи).
  check(near(LedgerUseCases::BandTotalScale(1.0, 1.0, kTrMin, kTrMax), 1.0),
        "LIQ-3: both axes neutral -> total 1.0 (no change)");

  // Только вола (liq=1): total == vol_scale (tr=[0.1,15] шире vr=[0.1,10] ⇒ не режет).
  check(near(LedgerUseCases::BandTotalScale(0.25, 1.0, kTrMin, kTrMax), 0.25),
        "vol-only (liq=1) -> total==vol_scale (tr wider, no cut)");
  check(near(LedgerUseCases::BandTotalScale(10.0, 1.0, kTrMin, kTrMax), 10.0),
        "vol-only at vr_max=10 -> 10 (within tr=15, D1 preserved)");

  // Только ликвидность (vol=1): total == liq_scale.
  check(near(LedgerUseCases::BandTotalScale(1.0, 2.0, kTrMin, kTrMax), 2.0),
        "liq-only (vol=1) -> total==liq_scale");

  // LIQ-2: обе оси в одну сторону, произведение за tr_max ⇒ клэмп tr_max (не vr·lr=20).
  check(near(LedgerUseCases::BandTotalScale(10.0, 2.0, kTrMin, kTrMax), kTrMax),
        "LIQ-2: vol_max*liq_max=20 -> clamp tr_max 15 (not 20)");

  // LIQ-2 низ: произведение ниже tr_min ⇒ клэмп tr_min.
  check(near(LedgerUseCases::BandTotalScale(0.1, 0.5, kTrMin, kTrMax), kTrMin),
        "LIQ-2: 0.1*0.5=0.05 -> clamp tr_min 0.1 (not 0.05)");

  // В диапазоне: произведение проходит без клэмпа.
  check(near(LedgerUseCases::BandTotalScale(2.0, std::sqrt(2.0), kTrMin, kTrMax),
             2.0 * std::sqrt(2.0)),
        "within range: 2*sqrt(2)~2.83 (no clamp)");

  if (g_fail == 0) {
    std::cout << "ALL PASS (ce_band_liq_scale_test)\n";
    return 0;
  }
  std::cout << g_fail << " FAILED\n";
  return 1;
}
