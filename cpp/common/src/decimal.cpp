#include "cex/common/decimal.hpp"

#include <cmath>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace cex::common {

static int64_t pow10_i(int32_t p) {
  int64_t r = 1;
  for (int32_t i = 0; i < p; ++i) r *= 10;
  return r;
}

static __int128 pow10_i128(int32_t p) {
  __int128 r = 1;
  for (int32_t i = 0; i < p; ++i) r *= 10;
  return r;
}

static __int128 round_div10(__int128 value) {
  const bool negative = value < 0;
  __int128 abs_value = negative ? -value : value;
  abs_value = (abs_value + 5) / 10;
  return negative ? -abs_value : abs_value;
}

static Decimal normalize_i128(__int128 units, int32_t scale) {
  while ((units > std::numeric_limits<int64_t>::max() ||
          units < std::numeric_limits<int64_t>::min()) &&
         scale > 0) {
    units = round_div10(units);
    --scale;
  }

  if (units > std::numeric_limits<int64_t>::max()) {
    units = std::numeric_limits<int64_t>::max();
  } else if (units < std::numeric_limits<int64_t>::min()) {
    units = std::numeric_limits<int64_t>::min();
  }

  return Decimal{static_cast<int64_t>(units), scale};
}

static __int128 align_units_i128(const Decimal& value, const int32_t target_scale) {
  __int128 units = value.units;
  if (target_scale <= value.scale) {
    return units;
  }
  return units * pow10_i128(target_scale - value.scale);
}

Decimal Decimal::from_proto(const fob::common::v1::Decimal& d) {
  return Decimal{d.units(), d.scale()};
}

fob::common::v1::Decimal Decimal::to_proto() const {
  fob::common::v1::Decimal d;
  d.set_units(units);
  d.set_scale(scale);
  return d;
}

std::string Decimal::to_string() const {
  // Render as signed decimal string without floating point.
  // Example: units=123456, scale=2 -> "1234.56"
  std::ostringstream oss;
  int64_t abs_units = std::llabs(units);
  int64_t int_part = abs_units / pow10_i(scale);
  int64_t frac_part = abs_units % pow10_i(scale);

  if (units < 0) oss << "-";
  oss << int_part;
  if (scale > 0) {
    oss << "." << std::setw(scale) << std::setfill('0') << frac_part;
  }
  return oss.str();
}

Decimal Decimal::mul(const Decimal& a, const Decimal& b) {
  // (a.units * 10^-a.scale) * (b.units * 10^-b.scale)
  // = (a.units*b.units) * 10^-(a.scale+b.scale)
  __int128 prod = static_cast<__int128>(a.units) * static_cast<__int128>(b.units);
  return normalize_i128(prod, a.scale + b.scale);
}

Decimal Decimal::add(const Decimal& a, const Decimal& b) {
  const int32_t scale = std::max(a.scale, b.scale);
  const __int128 lhs = align_units_i128(a, scale);
  const __int128 rhs = align_units_i128(b, scale);
  return normalize_i128(lhs + rhs, scale);
}

Decimal Decimal::sub(const Decimal& a, const Decimal& b) {
  const int32_t scale = std::max(a.scale, b.scale);
  const __int128 lhs = align_units_i128(a, scale);
  const __int128 rhs = align_units_i128(b, scale);
  return normalize_i128(lhs - rhs, scale);
}

int Decimal::cmp(const Decimal& a, const Decimal& b) {
  const int32_t scale = std::max(a.scale, b.scale);
  const __int128 lhs = align_units_i128(a, scale);
  const __int128 rhs = align_units_i128(b, scale);
  if (lhs < rhs) return -1;
  if (lhs > rhs) return 1;
  return 0;
}

Decimal Decimal::min(const Decimal& a, const Decimal& b) {
  return (cmp(a,b) <= 0) ? a : b;
}

Decimal Decimal::max(const Decimal& a, const Decimal& b) {
  return (cmp(a,b) >= 0) ? a : b;
}

Decimal Decimal::zero() {
  return Decimal{0, 0};
}

Decimal::operator double() const {
  return static_cast<double>(units) * std::pow(10, -scale);
}

} // namespace cex::common
