// =============================================================================
// Реализация арифметики Decimal (см. include/cex/common/decimal.hpp).
// Все операции работают через выравнивание scale, чтобы не терять точность
// меньшего из аргументов.
// =============================================================================

#include "cex/common/decimal.hpp"

#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace cex::common {

// Возведение 10 в неотрицательную степень в int64.
// Не используем std::pow, чтобы избежать ошибок с плавающей точкой и сохранить
// детерминированность результата на всех платформах.
static int64_t pow10_i(int32_t p) {
  int64_t r = 1;
  for (int32_t i = 0; i < p; ++i) r *= 10;
  return r;
}

// Тривиальный маппинг proto -> доменный тип.
Decimal Decimal::from_proto(const fob::common::v1::Decimal& d) {
  return Decimal{d.units(), d.scale()};
}

// Обратный маппинг — для отправки на проводе.
fob::common::v1::Decimal Decimal::to_proto() const {
  fob::common::v1::Decimal d;
  d.set_units(units);
  d.set_scale(scale);
  return d;
}

// Перевод в строку. Только для отладки/логов — на проводе всегда proto.
// Алгоритм: делим |units| на 10^scale, выводим целую и дробную части,
// дробную дополняем нулями слева до scale знаков. Знак минуса печатаем отдельно.
std::string Decimal::to_string() const {
  std::ostringstream oss;
  int64_t abs_units = std::llabs(units);
  int64_t int_part = abs_units / pow10_i(scale);
  int64_t frac_part = abs_units % pow10_i(scale);

  if (units < 0) oss << "-";
  oss << int_part;
  if (scale > 0) {
    // setw + setfill('0') гарантирует, что 0.05 не превратится в "0.5".
    oss << "." << std::setw(scale) << std::setfill('0') << frac_part;
  }
  return oss.str();
}

// Привести два числа к одному (большему) scale.
// Если у одного из них scale меньше — домножаем units на 10^(s - scale)
// и поднимаем его scale до общего. Возможно переполнение int64 при больших
// числах и большой разнице scale; для MVP допустимо.
static void align(const Decimal& a, const Decimal& b, Decimal& aa, Decimal& bb) {
  int32_t s = std::max(a.scale, b.scale);
  aa = a;
  bb = b;
  if (aa.scale < s) {
    aa.units *= pow10_i(s - aa.scale);
    aa.scale = s;
  }
  if (bb.scale < s) {
    bb.units *= pow10_i(s - bb.scale);
    bb.scale = s;
  }
}

// Умножение: (u1 * 10^-s1) * (u2 * 10^-s2) = (u1*u2) * 10^-(s1+s2).
// Используем __int128 для промежуточного произведения, чтобы уменьшить риск
// переполнения. В output всё равно кладём int64 — для MVP достаточно.
Decimal Decimal::mul(const Decimal& a, const Decimal& b) {
  __int128 prod = static_cast<__int128>(a.units) * static_cast<__int128>(b.units);
  Decimal out;
  out.scale = a.scale + b.scale;
  out.units = static_cast<int64_t>(prod); // MVP: проверка переполнения не делается
  return out;
}

// Сложение через выравнивание.
Decimal Decimal::add(const Decimal& a, const Decimal& b) {
  Decimal aa, bb;
  align(a, b, aa, bb);
  return Decimal{aa.units + bb.units, aa.scale};
}

// Вычитание через выравнивание.
Decimal Decimal::sub(const Decimal& a, const Decimal& b) {
  Decimal aa, bb;
  align(a, b, aa, bb);
  return Decimal{aa.units - bb.units, aa.scale};
}

// Сравнение через выравнивание: 1.0 == 1.00, поэтому сначала приводим к общему scale.
int Decimal::cmp(const Decimal& a, const Decimal& b) {
  Decimal aa, bb;
  align(a, b, aa, bb);
  if (aa.units < bb.units) return -1;
  if (aa.units > bb.units) return 1;
  return 0;
}

// min/max выражены через cmp — никакой "магии" сравнения через double.
Decimal Decimal::min(const Decimal& a, const Decimal& b) {
  return (cmp(a,b) <= 0) ? a : b;
}

Decimal Decimal::max(const Decimal& a, const Decimal& b) {
  return (cmp(a,b) >= 0) ? a : b;
}

} // namespace cex::common
