// ============================================================================
// ce_band_manual_test.cpp — ADR-081. Unit-тесты чистой функции LedgerUseCases::ResolveManualBand:
// ручная ширина полосы хеджа допустима только при включённом режиме и 0.1 <= lim <= mkt <= 1000 (тыс. USDT).
// ADR-080 (2026-10-20): плюс TransferFeeBps и FlatBandWidth — плата за перевод в пороге арбитражёра.
// Pure unit (без PG/Kafka): функции static и зависят только от аргументов и окружения.
// ============================================================================
#include <cmath>
#include <cstdlib>
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

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }
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


  // --- ADR-080 (уточнение 2026-10-20): плата за перевод между биржами в пороге арбитражёра.
  unsetenv("CE_TRANSFER_FEE_BPS_BTC");
  unsetenv("CE_TRANSFER_FEE_BPS_USDT");
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 2.5, "плата за перевод по умолчанию для актива = 2.5 б.п. (0.25 промилле)");
  check(LedgerUseCases::TransferFeeBps("USDT", "USDT") == 0.5, "плата за перевод по умолчанию для нумерария = 0.5 б.п. (0.05 промилле)");
  setenv("CE_TRANSFER_FEE_BPS_BTC", "4.0", 1);
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 4.0, "env CE_TRANSFER_FEE_BPS_BTC переопределяет");
  setenv("CE_TRANSFER_FEE_BPS_BTC", "abc", 1);
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 2.5, "не число => значение по умолчанию");
  setenv("CE_TRANSFER_FEE_BPS_BTC", "-1", 1);
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 2.5, "отрицательное => значение по умолчанию");
  setenv("CE_TRANSFER_FEE_BPS_BTC", "0", 1);
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 0.0, "явный ноль допустим (перевод бесплатный)");
  unsetenv("CE_TRANSFER_FEE_BPS_BTC");

  double zl = 0.0, zm = 0.0;
  LedgerUseCases::FlatBandWidth(1.8, 2.0, 10.0, 0.0, 1.0, 2.0, &zl, &zm);
  check(near(zl, 3.6) && near(zm, 18.0), "переводчик (плата 0): 3.6 / 18 — как до изменения");
  LedgerUseCases::FlatBandWidth(1.8, 2.0, 10.0, 2.5, 1.0, 2.0, &zl, &zm);
  check(near(zl, 8.1) && near(zm, 22.5), "арбитражёр (плата 2.5 б.п.): 1.8*(2+2.5)=8.1 и 1.8*(10+2.5)=22.5");
  LedgerUseCases::FlatBandWidth(0.1, 2.0, 10.0, 0.5, 1.0, 5.0, &zl, &zm);
  check(near(zl, 5.0) && near(zm, 5.0), "пол ширины удерживает порог, Z̄mkt не ниже Z̄lim");
  LedgerUseCases::FlatBandWidth(1.8, 2.0, 10.0, -3.0, 1.0, 2.0, &zl, &zm);
  check(near(zl, 3.6) && near(zm, 18.0), "отрицательная плата не уменьшает порог");
  LedgerUseCases::FlatBandWidth(1.8, 2.0, 10.0, 2.5, 1.0, 2.0, &zl, &zm);
  const double zl_arb = zl;
  LedgerUseCases::FlatBandWidth(1.8, 2.0, 10.0, 0.0, 1.0, 2.0, &zl, &zm);
  check(zl_arb > zl, "плата за перевод делает порог арбитражёра шире, чем у переводчика");


  // Доп. случаи из ревью: пустая строка, NaN/inf, мусор в хвосте, границы 1000/1001, независимость активов.
  setenv("CE_TRANSFER_FEE_BPS_BTC", "", 1);
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 2.5, "пустая строка => значение по умолчанию");
  setenv("CE_TRANSFER_FEE_BPS_BTC", "nan", 1);
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 2.5, "nan => значение по умолчанию");
  setenv("CE_TRANSFER_FEE_BPS_BTC", "inf", 1);
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 2.5, "inf => значение по умолчанию");
  setenv("CE_TRANSFER_FEE_BPS_BTC", "4.0x", 1);
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 2.5, "мусор в хвосте => значение по умолчанию");
  setenv("CE_TRANSFER_FEE_BPS_BTC", "1000", 1);
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 1000.0, "граница 1000 допустима");
  setenv("CE_TRANSFER_FEE_BPS_BTC", "1001", 1);
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 2.5, "больше 1000 => значение по умолчанию");
  setenv("CE_TRANSFER_FEE_BPS_BTC", "7.5", 1);
  unsetenv("CE_TRANSFER_FEE_BPS_ETH");
  check(LedgerUseCases::TransferFeeBps("BTC", "USDT") == 7.5 && LedgerUseCases::TransferFeeBps("ETH", "USDT") == 2.5,
        "переменные активов независимы (BTC задан, ETH по умолчанию)");
  unsetenv("CE_TRANSFER_FEE_BPS_BTC");

  LedgerUseCases::FlatBandWidth(1.8, 2.0, 10.0, std::nan(""), 1.0, 2.0, &zl, &zm);
  check(near(zl, 3.6) && near(zm, 18.0), "NaN плата не ломает порог (как 0)");
  LedgerUseCases::FlatBandWidth(1.8, 2.0, 10.0, std::numeric_limits<double>::infinity(), 1.0, 2.0, &zl, &zm);
  check(near(zl, 3.6) && near(zm, 18.0), "inf плата не ломает порог (как 0)");
  LedgerUseCases::FlatBandWidth(1.8, 2.0, 10.0, 2.5, 2.0, 2.0, &zl, &zm);
  check(near(zl, 16.2) && near(zm, 45.0), "rt = 2 удваивает порог: 1.8*(2+2.5)*2 и 1.8*(10+2.5)*2");
  // нумерарий-арбитражёр использует ту же формулу (только мейкерная граница): Z̄lim = k·(clim + плата USDT)
  LedgerUseCases::FlatBandWidth(1.8, 2.0, 0.0, 0.5, 1.0, 2.0, &zl, nullptr);
  check(near(zl, 4.5), "нумерарий: 1.8*(2+0.5)=4.5 (плата 0.5 б.п.)");


  // --- Пауза между выходами наружу: рост при неисполненных заявках (против петли повторной отправки).
  check(LedgerUseCases::BandHedgeCooldownMs(2000, 30000, 0) == 2000, "серия 0: базовая пауза 2 с");
  check(LedgerUseCases::BandHedgeCooldownMs(2000, 30000, 1) == 4000, "серия 1: 4 с");
  check(LedgerUseCases::BandHedgeCooldownMs(2000, 30000, 2) == 8000, "серия 2: 8 с");
  check(LedgerUseCases::BandHedgeCooldownMs(2000, 30000, 3) == 16000, "серия 3: 16 с");
  check(LedgerUseCases::BandHedgeCooldownMs(2000, 30000, 4) == 30000, "серия 4: 32 с обрезана до максимума 30 с");
  check(LedgerUseCases::BandHedgeCooldownMs(2000, 30000, 30) == 30000, "длинная серия не превышает максимум");
  check(LedgerUseCases::BandHedgeCooldownMs(2000, 0, 5) == 2000, "максимум 0 отключает рост");
  check(LedgerUseCases::BandHedgeCooldownMs(2000, 2000, 5) == 2000, "максимум равен базе — роста нет");
  check(LedgerUseCases::BandHedgeCooldownMs(2000, 30000, -3) == 2000, "отрицательная серия — база");
  check(LedgerUseCases::BandHedgeCooldownMs(0, 30000, 3) == 0, "нулевая база — без паузы");
  check(LedgerUseCases::BandHedgeCooldownMs(3000, 5000, 3) == 5000, "максимум чуть выше базы: 3 с, затем 5 с");


  // --- ADR-081: множитель порога арбитражёров.
  check(LedgerUseCases::ResolveArbScale(2.0) == 2.0 && LedgerUseCases::ResolveArbScale(0.1) == 0.1 && LedgerUseCases::ResolveArbScale(20.0) == 20.0,
        "множитель в пределах 0.1…20 принимается");
  check(LedgerUseCases::ResolveArbScale(0.05) == 1.0 && LedgerUseCases::ResolveArbScale(20.5) == 1.0, "вне пределов => 1");
  check(LedgerUseCases::ResolveArbScale(0.0) == 1.0 && LedgerUseCases::ResolveArbScale(-2.0) == 1.0, "0 и отрицательный => 1");
  check(LedgerUseCases::ResolveArbScale(std::nan("")) == 1.0 && LedgerUseCases::ResolveArbScale(std::numeric_limits<double>::infinity()) == 1.0,
        "NaN и inf => 1");
  zl = 8.1; zm = 22.5;
  LedgerUseCases::ApplyArbScale(2.0, 2.0, &zl, &zm);
  check(near(zl, 16.2) && near(zm, 45.0), "множитель 2: 8.1/22.5 => 16.2/45");
  zl = 8.1; zm = 22.5;
  LedgerUseCases::ApplyArbScale(1.0, 2.0, &zl, &zm);
  check(near(zl, 8.1) && near(zm, 22.5), "множитель 1 порог не меняет");
  zl = 8.1; zm = 22.5;
  LedgerUseCases::ApplyArbScale(0.1, 5.0, &zl, &zm);
  check(near(zl, 5.0) && zm >= zl, "множитель 0.1: пол удерживает Z̄lim, Z̄mkt не ниже Z̄lim");
  zl = 8.1;
  LedgerUseCases::ApplyArbScale(3.0, 2.0, &zl, nullptr);
  check(near(zl, 24.3), "нумерарий (одна граница): 8.1 * 3 = 24.3");
  zl = 8.1; zm = 22.5;
  LedgerUseCases::ApplyArbScale(std::nan(""), 2.0, &zl, &zm);
  check(near(zl, 8.1) && near(zm, 22.5), "NaN множитель = 1");

  if (g_fail) {
    std::cout << "ce_band_manual_test: " << g_fail << " FAIL\n";
    return 1;
  }
  std::cout << "ce_band_manual_test: OK\n";
  return 0;
}
