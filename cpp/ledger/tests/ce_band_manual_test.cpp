// ============================================================================
// ce_band_manual_test.cpp — ADR-081. Unit-тесты чистой функции LedgerUseCases::ResolveManualBand:
// ручная ширина полосы хеджа допустима только при включённом режиме и 0.1 <= lim <= mkt <= 1000 (тыс. USDT).
// Pure unit (без PG/Kafka): функция static и зависит только от аргументов.
// ============================================================================
#include <cmath>
#include <iostream>
#include <limits>
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
}  // namespace

int main() {
  double lim = -1.0, mkt = -1.0;

  check(!LedgerUseCases::ResolveManualBand(false, 10.0, 50.0, &lim, &mkt) && lim == -1.0 && mkt == -1.0,
        "режим выключен => false, выходные значения не тронуты");
  check(LedgerUseCases::ResolveManualBand(true, 10.0, 50.0, &lim, &mkt) && lim == 10.0 && mkt == 50.0,
        "включён и допустимо => значения переданы");
  check(LedgerUseCases::ResolveManualBand(true, 7.5, 7.5, &lim, &mkt) && lim == 7.5 && mkt == 7.5,
        "lim == mkt допустимо (одна граница)");
  check(LedgerUseCases::ResolveManualBand(true, 0.1, 1000.0, &lim, &mkt) && lim == 0.1 && mkt == 1000.0,
        "нижняя (0.1) и верхняя (1000) границы допустимы");
  check(!LedgerUseCases::ResolveManualBand(true, 0.0999, 50.0, nullptr, nullptr), "lim < 0.1 (пыль) недопустимо");
  check(!LedgerUseCases::ResolveManualBand(true, 0.0, 50.0, nullptr, nullptr), "lim = 0 недопустимо");
  check(!LedgerUseCases::ResolveManualBand(true, -5.0, 50.0, nullptr, nullptr), "lim < 0 недопустимо");
  check(!LedgerUseCases::ResolveManualBand(true, 20.0, 10.0, nullptr, nullptr), "mkt < lim недопустимо");
  check(!LedgerUseCases::ResolveManualBand(true, 10.0, 1000.5, nullptr, nullptr), "mkt > 1000 недопустимо");
  check(!LedgerUseCases::ResolveManualBand(true, 10.0, 100000.0, nullptr, nullptr), "прежняя граница 100000 теперь недопустима");
  check(!LedgerUseCases::ResolveManualBand(true, std::nan(""), 50.0, nullptr, nullptr), "NaN недопустимо");
  check(!LedgerUseCases::ResolveManualBand(true, 10.0, std::numeric_limits<double>::infinity(), nullptr, nullptr),
        "Inf недопустимо");

  // Зона исполнения хеджа (ревью ADR-081: при k_band = 0 ручной Z̄mkt не должен превращаться в «всегда тейкер»).
  check(LedgerUseCases::IsAggressiveBandZone(false, false, 5.0, 50.0), "нет привязки к комиссии, не ручной => всегда агрессив");
  check(!LedgerUseCases::IsAggressiveBandZone(false, true, 30.0, 50.0), "ручной без привязки к комиссии: |c| <= Z̄mkt => мейкер");
  check(LedgerUseCases::IsAggressiveBandZone(false, true, 60.0, 50.0), "ручной без привязки к комиссии: |c| > Z̄mkt => тейкер");
  check(!LedgerUseCases::IsAggressiveBandZone(true, false, 10.0, 18.0), "привязка к комиссии: между границами => мейкер");
  check(LedgerUseCases::IsAggressiveBandZone(true, false, 19.0, 18.0), "привязка к комиссии: выше Z̄mkt => тейкер");
  check(!LedgerUseCases::IsAggressiveBandZone(true, true, 50.0, 50.0), "граница |c| == Z̄mkt => ещё мейкер");

  if (g_fail) {
    std::cout << "ce_band_manual_test: " << g_fail << " FAIL\n";
    return 1;
  }
  std::cout << "ce_band_manual_test: OK\n";
  return 0;
}
