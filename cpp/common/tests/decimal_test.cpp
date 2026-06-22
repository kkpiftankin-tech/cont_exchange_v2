// ============================================================================
// decimal_test.cpp — unit-тест cex::common::Decimal::div (F-09, §9 money).
// Hand-rolled harness. Проверяет точность, half-up округление, знак, /0.
// ============================================================================

#include <iostream>
#include <stdexcept>

#include "cex/common/decimal.hpp"

namespace {

using cex::common::Decimal;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

// Проверяет, что div(a,b,scale) даёт ровно Decimal{units,scale}.
bool eq_div(const Decimal& a, const Decimal& b, std::int32_t scale,
            std::int64_t want_units, const char* msg) {
  const Decimal r = Decimal::div(a, b, scale);
  return expect(Decimal::cmp(r, Decimal{want_units, scale}) == 0, msg);
}

}  // namespace

int main() {
  bool ok = true;

  // Точное деление: 10 / 4 = 2.50.
  ok = eq_div(Decimal{10, 0}, Decimal{4, 0}, 2, 250, "10/4 @2 = 2.50") && ok;
  // Половина округляется вверх (от нуля): 7 / 2 = 3.5 → @0 = 4.
  ok = eq_div(Decimal{7, 0}, Decimal{2, 0}, 0, 4, "7/2 @0 = 4 (half-up)") && ok;
  // 1/3 @4 = 0.3333 (5-й знак 3 → вниз).
  ok = eq_div(Decimal{1, 0}, Decimal{3, 0}, 4, 3333, "1/3 @4 = 0.3333") && ok;
  // 2/3 @4 = 0.6667 (5-й знак 6 → вверх).
  ok = eq_div(Decimal{2, 0}, Decimal{3, 0}, 4, 6667, "2/3 @4 = 0.6667") && ok;
  // Знак: -10 / 4 @1 = -2.5.
  ok = eq_div(Decimal{-10, 0}, Decimal{4, 0}, 1, -25, "-10/4 @1 = -2.5") && ok;
  // Разные scale у операндов: 2.0 / 0.6 @4 = 3.3333.
  ok = eq_div(Decimal{20, 1}, Decimal{6, 1}, 4, 33333, "2.0/0.6 @4 = 3.3333") && ok;

  // Сохранение пропорции ног (как в grouped solver): e_i = Q_b · ρ_i / ρ_b.
  // Q_b=5, ρ_i=0.4, ρ_b=0.6 → 5*0.4/0.6 = 2/0.6 = 3.3333.
  {
    const Decimal qb_ri = Decimal::mul(Decimal{5, 0}, Decimal{4, 1});  // 5 * 0.4 = 2.0
    ok = eq_div(qb_ri, Decimal{6, 1}, 4, 33333, "leg ratio e_i = 5*0.4/0.6 = 3.3333") && ok;
  }

  // Деление на ноль → исключение.
  {
    bool threw = false;
    try {
      (void)Decimal::div(Decimal{1, 0}, Decimal{0, 0}, 2);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    ok = expect(threw, "div by zero throws std::invalid_argument") && ok;
  }

  if (ok) { std::cout << "decimal_test: ALL PASSED\n"; return 0; }
  return 1;
}
