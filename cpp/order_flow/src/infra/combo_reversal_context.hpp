#pragma once
// ============================================================================
// combo_reversal_context.hpp — F-09 MVP-6 slice 3b (T-F09-067, ADR-040).
//
// Лёгкие DTO для reverse_internal (без pqxx) — чтобы use case и его unit-тест не
// тянули PostgreSQL-зависимости. Репозиторий (postgres_combo_order_repository)
// наполняет их из combo_order_legs + combo_orders.
// ============================================================================

#include <string>
#include <vector>

#include "cex/common/decimal.hpp"

namespace cex::order_flow::infra {

/// Одна internal-нога с ненулевым внутренним fill — вход для reverse_internal.
/// Цены/rate берутся из исходной ноги (band ценоинвариантен к стороне — это
/// диапазон, а не directional limit).
struct ReversalLegRow {
  std::string leg_id;
  std::string instrument_symbol;  ///< "BASE/QUOTE"
  bool is_buy{true};              ///< исходная сторона ноги (реверс — обратная)
  cex::common::Decimal p_low{};
  cex::common::Decimal p_high{};
  cex::common::Decimal q_rate{};
  cex::common::Decimal filled_cum{};
};

/// Контекст разворота combo: владелец + internal-ноги с filled_cum>0.
struct ComboReversalContext {
  std::string user_id;
  std::string account_id;
  std::vector<ReversalLegRow> legs;
};

}  // namespace cex::order_flow::infra
