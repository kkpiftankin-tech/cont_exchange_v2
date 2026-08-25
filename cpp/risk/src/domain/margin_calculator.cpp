// ============================================================================
// margin_calculator.cpp — реализация формул маржи (см. margin_calculator.hpp).
//
// Все суммы аккумулируются через Decimal::add / Decimal::mul (CLAUDE.md §9).
// abs() для qty реализован через cmp с нулём (positions.quantity >= 0 по схеме,
// но защищаемся на случай отрицательного входа).
// ============================================================================

#include "domain/margin_calculator.hpp"

namespace cex::risk::domain {

namespace {

// Модуль Decimal: если value < 0 → 0 - value, иначе value. Используем для
// |quantity| на случай отрицательного qty (схема гарантирует >= 0, но domain
// не должен ломаться на грязном входе).
Decimal Abs(const Decimal& value) {
  if (Decimal::cmp(value, Decimal::zero()) < 0) {
    return Decimal::sub(Decimal::zero(), value);
  }
  return value;
}

}  // namespace

MarginResult ComputeMargin(const std::vector<MarginPosition>& positions,
                           const MarginRates& rates,
                           const Decimal& free_balance,
                           const Decimal& reserved) {
  MarginResult result;

  Decimal initial_margin = Decimal::zero();
  Decimal maintenance_margin = Decimal::zero();
  Decimal unrealized_total = Decimal::zero();

  for (const auto& pos : positions) {
    // notional_i = |qty_i| * mark_i (mark-to-market value позиции).
    const Decimal notional = Decimal::mul(Abs(pos.quantity), pos.mark_price);

    // initial/maintenance margin как доля notional.
    initial_margin = Decimal::add(initial_margin,
                                  Decimal::mul(notional, rates.initial_margin_rate));
    maintenance_margin = Decimal::add(
        maintenance_margin, Decimal::mul(notional, rates.maintenance_margin_rate));

    unrealized_total = Decimal::add(unrealized_total, pos.unrealized_pnl);
  }

  // free_collateral = Σ free_balance(quote) + Σ unrealized_pnl.
  const Decimal free_collateral = Decimal::add(free_balance, unrealized_total);

  result.initial_margin = initial_margin;
  result.maintenance_margin = maintenance_margin;
  result.free_collateral = free_collateral;
  result.reserved_collateral = reserved;

  // Margin state (SEQ-RISK-001 «Margin state → risk_flags»).
  result.throttled = Decimal::cmp(free_collateral, initial_margin) < 0;
  result.margin_call = Decimal::cmp(free_collateral, maintenance_margin) < 0;
  result.liquidation = Decimal::cmp(free_collateral, Decimal::zero()) <= 0;

  return result;
}

}  // namespace cex::risk::domain
