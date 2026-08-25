// ============================================================================
// margin_calculator_test.cpp — F-06 (T-F06-060 U9/U10). Hand-rolled harness
// (тот же стиль, что grouped_risk_check_test): чистый domain, Decimal only.
//
// Покрывает:
//   U9  — initial_margin = notional * initial_margin_rate (агрегат по позициям).
//   U10 — margin_call при free_collateral < maintenance_margin.
//   + sanity: liquidation при free_collateral <= 0; flat-вход (нет позиций) → 0.
// ============================================================================

#include <iostream>
#include <string>
#include <vector>

#include "domain/margin_calculator.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::risk::domain;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

// Decimal равенство по числовому значению (cmp == 0), независимо от scale.
bool eq(const Decimal& a, const Decimal& b) { return Decimal::cmp(a, b) == 0; }

d::MarginPosition Pos(const std::string& sym, std::int64_t qty,
                      std::int64_t mark, const Decimal& upnl = Decimal::zero()) {
  d::MarginPosition p;
  p.symbol = sym;
  p.quantity = Decimal{qty, 0};
  p.mark_price = Decimal{mark, 0};
  p.unrealized_pnl = upnl;
  return p;
}

}  // namespace

int main() {
  bool ok = true;

  // Ставки: initial 10% (0.1), maintenance 5% (0.05).
  d::MarginRates rates;
  rates.initial_margin_rate = Decimal{1000, 4};      // 0.1000
  rates.maintenance_margin_rate = Decimal{500, 4};   // 0.0500

  // --- U9: initial_margin = Σ notional_i * rate ---------------------------
  // Позиция: 2 BTC @ mark 50000 → notional = 100000.
  // initial = 100000 * 0.1 = 10000; maintenance = 100000 * 0.05 = 5000.
  {
    std::vector<d::MarginPosition> positions = {Pos("BTCUSDT", 2, 50000)};
    const auto r = d::ComputeMargin(positions, rates,
                                    /*free_balance=*/Decimal{20000, 0},
                                    /*reserved=*/Decimal{1000, 0});
    ok &= expect(eq(r.initial_margin, Decimal{10000, 0}),
                 "U9: initial_margin = notional * 0.1 = 10000");
    ok &= expect(eq(r.maintenance_margin, Decimal{5000, 0}),
                 "U9: maintenance_margin = notional * 0.05 = 5000");
    // free_collateral = free_balance + unrealized = 20000 + 0.
    ok &= expect(eq(r.free_collateral, Decimal{20000, 0}),
                 "U9: free_collateral = 20000");
    ok &= expect(eq(r.reserved_collateral, Decimal{1000, 0}),
                 "U9: reserved_collateral = 1000");
    // free (20000) >= initial (10000) → не throttled, не margin_call.
    ok &= expect(!r.throttled, "U9: not throttled");
    ok &= expect(!r.margin_call, "U9: not margin_call");
    ok &= expect(!r.liquidation, "U9: not liquidation");
  }

  // --- U9b: агрегация по нескольким позициям + unrealized_pnl в collateral --
  // BTC: 1 @ 50000 → notional 50000. ETH: 10 @ 3000 → notional 30000.
  // total notional 80000. initial = 8000, maintenance = 4000.
  // free = free_balance 5000 + uPnL (-1000) = 4000.
  {
    std::vector<d::MarginPosition> positions = {
        Pos("BTCUSDT", 1, 50000, Decimal{-500, 0}),
        Pos("ETHUSDT", 10, 3000, Decimal{-500, 0}),
    };
    const auto r = d::ComputeMargin(positions, rates,
                                    /*free_balance=*/Decimal{5000, 0},
                                    /*reserved=*/Decimal::zero());
    ok &= expect(eq(r.initial_margin, Decimal{8000, 0}),
                 "U9b: initial_margin = 80000 * 0.1 = 8000");
    ok &= expect(eq(r.maintenance_margin, Decimal{4000, 0}),
                 "U9b: maintenance_margin = 80000 * 0.05 = 4000");
    ok &= expect(eq(r.free_collateral, Decimal{4000, 0}),
                 "U9b: free_collateral = 5000 + (-1000) = 4000");
    // free (4000) < initial (8000) → throttled; free (4000) == maintenance
    // (4000) → NOT margin_call (strict <).
    ok &= expect(r.throttled, "U9b: throttled (free < initial)");
    ok &= expect(!r.margin_call, "U9b: not margin_call (free == maintenance)");
  }

  // --- U10: margin_call при free_collateral < maintenance_margin ----------
  // notional 100000 → maintenance 5000. free = 4000 < 5000 → margin_call.
  {
    std::vector<d::MarginPosition> positions = {Pos("BTCUSDT", 2, 50000)};
    const auto r = d::ComputeMargin(positions, rates,
                                    /*free_balance=*/Decimal{4000, 0},
                                    /*reserved=*/Decimal::zero());
    ok &= expect(r.margin_call, "U10: margin_call (free 4000 < maintenance 5000)");
    ok &= expect(r.throttled, "U10: throttled (free < initial)");
    ok &= expect(!r.liquidation, "U10: not liquidation (free > 0)");
  }

  // --- liquidation: free_collateral <= 0 ----------------------------------
  {
    std::vector<d::MarginPosition> positions = {Pos("BTCUSDT", 2, 50000)};
    // free_balance 1000 + uPnL -1000 = 0 → liquidation.
    positions[0].unrealized_pnl = Decimal{-1000, 0};
    const auto r = d::ComputeMargin(positions, rates,
                                    /*free_balance=*/Decimal{1000, 0},
                                    /*reserved=*/Decimal::zero());
    ok &= expect(r.liquidation, "liquidation (free_collateral <= 0)");
    ok &= expect(r.margin_call, "liquidation also flags margin_call");
  }

  // --- flat / no positions → нулевая маржа, нет флагов --------------------
  {
    std::vector<d::MarginPosition> positions;
    const auto r = d::ComputeMargin(positions, rates,
                                    /*free_balance=*/Decimal{100, 0},
                                    /*reserved=*/Decimal::zero());
    ok &= expect(eq(r.initial_margin, Decimal::zero()), "flat: initial_margin = 0");
    ok &= expect(eq(r.maintenance_margin, Decimal::zero()), "flat: maintenance = 0");
    ok &= expect(!r.throttled && !r.margin_call && !r.liquidation,
                 "flat: no risk flags (free 100 >= 0)");
  }

  if (ok) {
    std::cout << "margin_calculator_test: ALL PASSED\n";
    return 0;
  }
  std::cerr << "margin_calculator_test: FAILURES\n";
  return 1;
}
