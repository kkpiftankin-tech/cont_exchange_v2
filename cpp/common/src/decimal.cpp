#include "cex/common/decimal.hpp"

#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace cex::common {

static int64_t pow10_i(int32_t p) {
  int64_t r = 1;
  for (int32_t i = 0; i < p; ++i) r *= 10;
  return r;
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

Decimal Decimal::mul(const Decimal& a, const Decimal& b) {
  // (a.units * 10^-a.scale) * (b.units * 10^-b.scale)
  // = (a.units*b.units) * 10^-(a.scale+b.scale)
  __int128 prod = static_cast<__int128>(a.units) * static_cast<__int128>(b.units);
  Decimal out;
  out.scale = a.scale + b.scale;
  out.units = static_cast<int64_t>(prod); // MVP: ignore overflow checks
  return out;
}

Decimal Decimal::add(const Decimal& a, const Decimal& b) {
  Decimal aa, bb;
  align(a, b, aa, bb);
  return Decimal{aa.units + bb.units, aa.scale};
}

Decimal Decimal::sub(const Decimal& a, const Decimal& b) {
  Decimal aa, bb;
  align(a, b, aa, bb);
  return Decimal{aa.units - bb.units, aa.scale};
}

int Decimal::cmp(const Decimal& a, const Decimal& b) {
  Decimal aa, bb;
  align(a, b, aa, bb);
  if (aa.units < bb.units) return -1;
  if (aa.units > bb.units) return 1;
  return 0;
}

Decimal Decimal::min(const Decimal& a, const Decimal& b) {
  return (cmp(a,b) <= 0) ? a : b;
}

Decimal Decimal::max(const Decimal& a, const Decimal& b) {
  return (cmp(a,b) >= 0) ? a : b;
}

} // namespace cex::common
