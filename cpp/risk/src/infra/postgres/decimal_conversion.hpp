#pragma once
// ============================================================================
// decimal_conversion.hpp — мост PostgreSQL NUMERIC(text) <-> cex::common::Decimal
// для risk-сервиса (F-06, T-F06-031).
//
// Зеркало cpp/matching/src/infra/postgres/decimal_conversion.{hpp,cpp} —
// дублируется здесь в risk-namespace, чтобы не вводить cross-service
// common-utils рефакторинг ради одной фичи (тот же приём, что в PreHedgeCheck
// с ParseDecimalString). Логика идентична: ParsePgNumeric (PG text → Decimal)
// и ToPgNumeric (Decimal → text для exec_params). См. matching-версию для
// истории бага PR-F02-002 (trailing-zeros overflow guard).
// ============================================================================

#include <string>
#include <string_view>

#include "cex/common/decimal.hpp"

namespace cex::risk::infra::postgres {

// PG NUMERIC text → Decimal. Стрипает trailing zeros до парсинга (overflow
// guard). Бросает std::invalid_argument / std::overflow_error на грязном входе.
cex::common::Decimal ParsePgNumeric(std::string_view text_value);

// Decimal → канонический text для подстановки в exec_params ($N).
std::string ToPgNumeric(const cex::common::Decimal& value);

}  // namespace cex::risk::infra::postgres
