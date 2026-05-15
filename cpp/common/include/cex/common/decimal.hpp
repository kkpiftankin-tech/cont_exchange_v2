#pragma once
#include <cstdint>
#include <string>
#include "fob/common/v1/common.pb.h"

namespace cex::common {

// Fixed-point decimal value: units * 10^(-scale).
// We keep the same representation as in proto so it's easy to map.
struct Decimal {
  int64_t units{0};
  int32_t scale{0};

  static Decimal from_proto(const fob::common::v1::Decimal& d);
  fob::common::v1::Decimal to_proto() const;

  // Convert to human readable string (debug only).
  std::string to_string() const;

  // Multiply two fixed point values (may overflow if numbers are huge; ok for MVP/dev).
  static Decimal mul(const Decimal& a, const Decimal& b);

  // Align two decimals to same scale and add: c = a + b.
  // For simplicity we align to max(scale) => more precise representation.
  static Decimal add(const Decimal& a, const Decimal& b);

  // Align and subtract: c = a - b.
  static Decimal sub(const Decimal& a, const Decimal& b);

  // min(a,b) with same scale alignment
  static Decimal min(const Decimal& a, const Decimal& b);

  // max(a,b)
  static Decimal max(const Decimal& a, const Decimal& b);

  // compare: -1,0,1
  static int cmp(const Decimal& a, const Decimal& b);

  // zero: Decimal{0, 0}
  static Decimal zero();

  // double(units * 10^(-scale))
  explicit operator double() const;
};

}  // namespace cex::common
