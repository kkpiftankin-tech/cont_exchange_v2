#pragma once
// ============================================================================
// margin_calculator.hpp — pure-domain расчёт маржи для F-06 (SEQ-RISK-001).
//
// Назначение и физический смысл:
//   После каждого батча Risk строит RiskSnapshot для затронутых пользователей.
//   Этот модуль — чистая (без I/O, без Kafka, без PG) реализация формул маржи
//   из SEQ-RISK-001-build-risk-snapshot.md, чтобы её можно было unit-тестить
//   (U9/U10) детерминированно.
//
// Формулы (domain), см. SEQ-RISK-001 «Margin formulas»:
//   notional_i          = |quantity_i| * mark_price_i        (per open position)
//   mtm_i               = notional_i                          (mark-to-market value)
//   initial_margin_i    = notional_i * initial_margin_rate
//   maintenance_margin_i= notional_i * maintenance_margin_rate
//   free_collateral     = Σ free_balance(quote) + Σ unrealized_pnl
//   reserved_collateral = Σ reserved_balance
//
// Margin state → risk_flags (SEQ-RISK-001 «Margin state»):
//   throttled   := free_collateral < initial_margin_total
//   margin_call := free_collateral < maintenance_margin_total
//   liquidation := free_collateral <= 0
//
// Финансовая дисциплина (CLAUDE.md §9): все величины — Decimal, никаких double.
// ============================================================================

#include <string>
#include <vector>

#include "cex/common/decimal.hpp"

namespace cex::risk::domain {

using cex::common::Decimal;

// Одна открытая позиция пользователя (side != flat) с её mark price.
// mark_price — clear price из BatchResult по символу позиции; если clear price
// для символа отсутствует (stale), caller подставляет последний известный
// (например avg_entry_price) и выставляет mark_stale.
struct MarginPosition {
  std::string symbol;
  Decimal quantity{Decimal::zero()};        // абсолютное qty (positions.quantity >= 0)
  Decimal mark_price{Decimal::zero()};      // clear/last price для символа
  Decimal unrealized_pnl{Decimal::zero()};  // mark-to-market PnL из positions
};

// Ставки маржи (из risk_limits; с разумным дефолтом если строки нет).
struct MarginRates {
  Decimal initial_margin_rate{Decimal::zero()};      // доля notional
  Decimal maintenance_margin_rate{Decimal::zero()};  // доля notional
};

// Результат расчёта маржи — то, что пишется в risk_snapshots и отдаётся в
// RiskSnapshot proto.
struct MarginResult {
  Decimal initial_margin{Decimal::zero()};
  Decimal maintenance_margin{Decimal::zero()};
  Decimal free_collateral{Decimal::zero()};
  Decimal reserved_collateral{Decimal::zero()};
  bool throttled{false};
  bool margin_call{false};
  bool liquidation{false};
};

// ComputeMargin — суммирует margin по всем позициям, складывает коллатерал и
// вычисляет margin-state флаги. Pure: один и тот же вход → один и тот же выход.
//   positions      — открытые позиции с mark price.
//   rates          — initial/maintenance ставки.
//   free_balance   — Σ free_balance (quote collateral) из accounts.
//   reserved       — Σ reserved_balance из accounts.
MarginResult ComputeMargin(const std::vector<MarginPosition>& positions,
                           const MarginRates& rates,
                           const Decimal& free_balance,
                           const Decimal& reserved);

}  // namespace cex::risk::domain
