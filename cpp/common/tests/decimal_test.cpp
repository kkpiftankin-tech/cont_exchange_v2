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

  // Числитель × 10^k выходит за __int128: раньше знак результата менялся молча (книга агента, avg_fair < 0), теперь масштаб снижается.
  {
    // 80 078,88203121129 (масштаб 14) × 0,25 (масштаб 18, 2,5e17 единиц) → оборот ≈ 20 тыс.: числитель 2e18, k = 16 − 14 + 18 = 20.
    const Decimal notional{2000788820312112900LL, 14};   // 20 007,888203121129
    const Decimal total{250000000000000000LL, 18};       // 0,25
    const Decimal r = Decimal::div(notional, total, 16);
    ok = expect(r.units > 0, "div: большой числитель при высоком масштабе знаменателя не меняет знак") && ok;
    // точное значение 80031,552812484516 в масштабе 14 (масштаб результата снижен с 16: числитель × 10^20 не помещался)
    ok = expect(Decimal::cmp(r, Decimal{8003155281248451600LL, 14}) == 0, "div: 20007,888203121129 / 0,25 = 80031,552812484516 точно") && ok;
    ok = expect(r.scale <= 16, "div: масштаб результата не больше запрошенного") && ok;
    const Decimal neg = Decimal::div(Decimal{-2000788820312112900LL, 14}, total, 16);
    ok = expect(neg.units < 0 && static_cast<double>(neg) < -80031.0, "div: знак отрицательного числителя сохраняется") && ok;
  }

  // Без риска переполнения масштаб результата — ровно запрошенный (поведение прежнее).
  {
    const Decimal r = Decimal::div(Decimal{10, 0}, Decimal{4, 0}, 2);
    ok = expect(r.scale == 2 && r.units == 250, "div: без переполнения масштаб результата равен запрошенному") && ok;
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
