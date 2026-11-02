// ============================================================================
// ce_agent_band_test.cpp — F-18 v2 · Э3 (T-F18-301/302, ADR-061 §4, CHK-33..36).
// Hand-rolled harness (в стиле grouped_risk_check_test — без GoogleTest).
// Полоса ±q: внутри полосы нет эмиссии; за полосой — избыток (|c|−q)·sign(c);
// уже отправленное (in_flight) повторно не шлём. Всё в Decimal (§9).
// ============================================================================

#include <iostream>

#include "cex/common/decimal.hpp"
#include "domain/ce_agent_band.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::risk::domain;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

// Целое (scale=0) значение позиции/полосы.
Decimal I(std::int64_t v) { return Decimal{v, 0}; }

bool eq(const Decimal& a, std::int64_t v) { return Decimal::cmp(a, Decimal{v, 0}) == 0; }

}  // namespace

int main() {
  bool ok = true;

  // 1) |c| ≤ q → нет эмиссии.
  {
    auto r = d::ComputeBandEmission(I(10), I(0), I(18));
    ok &= expect(!r.emit, "inside band: no emit");
    ok &= expect(eq(r.signed_qty, 0), "inside band: qty 0");
  }

  // 2) Ровно на границе → строгое >, нет эмиссии.
  {
    auto r = d::ComputeBandEmission(I(18), I(0), I(18));
    ok &= expect(!r.emit, "on boundary: no emit");
  }

  // 3) |c| > q, in_flight=0 → SELL избыток (25−18)=7.
  {
    auto r = d::ComputeBandEmission(I(25), I(0), I(18));
    ok &= expect(r.emit, "positive excess: emit");
    ok &= expect(eq(r.signed_qty, 7), "positive excess: +7 (SELL)");
  }

  // 4) Отрицательная позиция → BUY (обратный знак).
  {
    auto r = d::ComputeBandEmission(I(-25), I(0), I(18));
    ok &= expect(r.emit, "negative excess: emit");
    ok &= expect(eq(r.signed_qty, -7), "negative excess: -7 (BUY)");
  }

  // 5) Guard T-F18-302: избыток уже в in_flight → повторно не шлём.
  {
    auto r = d::ComputeBandEmission(I(25), I(7), I(18));
    ok &= expect(!r.emit, "excess covered by in_flight: no reemit");
  }

  // 6) Частично покрыт → шлём остаток (30−18−7)=5.
  {
    auto r = d::ComputeBandEmission(I(30), I(7), I(18));
    ok &= expect(r.emit, "partially covered: emit");
    ok &= expect(eq(r.signed_qty, 5), "partially covered: +5");
  }

  // 7) Отрицательный q → трактуется как 0.
  {
    auto r = d::ComputeBandEmission(I(5), I(0), I(-3));
    ok &= expect(r.emit, "negative band: emit");
    ok &= expect(eq(r.signed_qty, 5), "negative band: +5");
  }

  if (ok) { std::cout << "ce_agent_band_test: ALL PASSED\n"; return 0; }
  std::cerr << "ce_agent_band_test: FAILURES\n";
  return 1;
}
