// ============================================================================
// compensation_reversal_test.cpp — F-09 MVP-6 (ADR-039 / ADR-040 §4).
// Hand-rolled harness. long→sell, short→buy, filled 0 skipped, multi-leg.
// ============================================================================

#include <iostream>

#include "cex/common/compensation_reversal.hpp"

namespace {
using cex::common::Decimal;
namespace c = cex::common;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

c::ReversalLeg Leg(const std::string& sym, bool is_buy, std::int64_t filled) {
  return c::ReversalLeg{sym, is_buy, Decimal{filled, 0}};
}
}  // namespace

int main() {
  bool ok = true;

  // Long BTC (buy 10) → reversal sell 10; short ETH (sell 5) → reversal buy 5;
  // zero-fill SOL → пропущена.
  const auto rev = c::ComputeReversals(
      {Leg("BTCUSDT", true, 10), Leg("ETHUSDT", false, 5), Leg("SOLUSDT", true, 0)});

  ok = expect(rev.size() == 2, "two reversals (zero-fill skipped)") && ok;
  if (rev.size() == 2) {
    ok = expect(rev[0].instrument_symbol == "BTCUSDT" && !rev[0].is_buy &&
                    Decimal::cmp(rev[0].qty, Decimal{10, 0}) == 0,
                "long BTC → sell 10") && ok;
    ok = expect(rev[1].instrument_symbol == "ETHUSDT" && rev[1].is_buy &&
                    Decimal::cmp(rev[1].qty, Decimal{5, 0}) == 0,
                "short ETH → buy 5") && ok;
  }

  // Все ноги без fill → пустой результат.
  ok = expect(c::ComputeReversals({Leg("BTCUSDT", true, 0)}).empty(),
              "all zero-fill → no reversals") && ok;

  if (ok) { std::cout << "compensation_reversal_test: ALL PASSED\n"; return 0; }
  return 1;
}
