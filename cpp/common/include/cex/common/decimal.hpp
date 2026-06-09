#pragma once
// ============================================================================
// decimal.hpp — fixed-point денежный тип проекта.
//
// Назначение и физический смысл:
//   Decimal представляет точное десятичное число формата value = units * 10^(-scale).
//   Это единственный допустимый тип для денежных величин в доменной логике,
//   ledger, risk, matching settlement и persistence — см. CLAUDE.md §9
//   ("Запрещено использовать double/float для денежных величин").
//
//   Пример: Decimal{units=12345, scale=2} = 123.45
//           Decimal{units=-1,    scale=6} = -0.000001
//           Decimal{units=0,     scale=0} = 0
//
// Почему так, а не decimal128 / boost::multiprecision / arbitrary-precision:
//   1. Тривиальная биекция с proto fob.common.v1.Decimal (one-to-one mapping).
//   2. int64 + int32 — 12 байт, помещается в кэш-линию, копирование O(1).
//   3. Отсутствие аллокаций (struct lives on stack).
//   4. Достаточно для всех реалистичных amount/price на ближайшие годы
//      (int64 = ±9.2e18 единиц; при scale=18 это диапазон до ~9.2 BTC,
//       при scale=8 — до 9.2e10 USDT).
//
// Известные ограничения (см. комментарии mul/add):
//   * mul() не защищён от int64-переполнения для очень больших чисел —
//     OK для MVP, но в проде нужен __int128 guard (см. decimal_conversion.cpp
//     как пример overflow detection).
//   * sub() с разными scale делает align к max(scale) — потенциально может
//     потерять точность, если одно из чисел уже близко к int64-границе.
//
// Контекст использования:
//   * order_flow: total_qty, price_low, price_high, max_speed во FlowOrder
//   * matching: clear_prices, executed_rates, fills, residual diagnostics
//   * ledger: balances, reservations, position quantities
//   * risk: notional checks, margin, exposure limits
//   * venues: fill prices/quantities из execution reports
// ============================================================================

#include <cstdint>     // int64_t / int32_t — гарантированно-широкие целые типы
#include <string>      // std::string — для to_string() (debug-вывод)
#include "fob/common/v1/common.pb.h"  // generated proto — для from_proto/to_proto моста

namespace cex::common {

// Fixed-point decimal value: units * 10^(-scale).
// We keep the same representation as in proto so it's easy to map.
struct Decimal {
  int64_t units{0};   ///< Целочисленное значение в "единицах" минимального разряда.
                       ///<  Default-initialized в 0 через {} (C++11 brace-init).
  int32_t scale{0};   ///< Число десятичных знаков после точки. scale >= 0.
                       ///<  scale=0 — целое число; scale=8 — типичный BTC scale.

  /// Перевод из proto-сообщения. proto.Decimal имеет ровно ту же структуру
  /// (int64 units + int32 scale), так что mapping тривиален. Используется при
  /// разборе входящих gRPC-запросов и Kafka-сообщений.
  static Decimal from_proto(const fob::common::v1::Decimal& d);

  /// Обратное преобразование — для исходящих proto-сообщений. const-метод:
  /// не модифицирует *this, может вызываться на const-объекте.
  fob::common::v1::Decimal to_proto() const;

  // Convert to human readable string (debug only).
  /// Канонический формат: "[-]integer.fractional" без trailing zeros.
  /// Пример: Decimal{12345, 2}.to_string() == "123.45".
  /// Используется ToPgNumeric() для подстановки в SQL exec_params, а также
  /// в structured-логах. НЕ предназначен для пользовательского отображения
  /// (нет thousands separator, нет локали) — это сериализация для машин.
  std::string to_string() const;

  // Multiply two fixed point values (may overflow if numbers are huge; ok for MVP/dev).
  /// Результат: result.units = a.units * b.units, result.scale = a.scale + b.scale.
  /// Математика: (a_u * 10^-a_s) * (b_u * 10^-b_s) = (a_u*b_u) * 10^-(a_s+b_s).
  /// ВНИМАНИЕ: при крупных units возможно int64 overflow — для prod-кода
  /// рекомендуется добавить __int128 guard (см. ParsePgNumeric как образец).
  static Decimal mul(const Decimal& a, const Decimal& b);

  // Align two decimals to same scale and add: c = a + b.
  // For simplicity we align to max(scale) => more precise representation.
  /// Алгоритм: scale = max(a.scale, b.scale); умножаем units меньшего scale на
  /// 10^(scale_diff), затем складываем. Aлайн к max(scale) сохраняет точность,
  /// но может вызвать переполнение для near-int64-max значений.
  static Decimal add(const Decimal& a, const Decimal& b);

  // Align and subtract: c = a - b.
  /// Та же логика что и add(), но вычитание. Используется в balance updates,
  /// reservation release, position adjustments.
  static Decimal sub(const Decimal& a, const Decimal& b);

  // Divide a/b with an explicit result scale (half-up, round away from zero).
  /// Делит a/b и округляет до result_scale знаков после точки (half-up,
  /// округление от нуля). result_scale обязателен и явный — у деления нет
  /// «естественного» scale (в отличие от mul/add). Бросает std::invalid_argument
  /// при делении на ноль. Нужен для grouped vector solver (F-09): пропорции ног
  /// e_i = Q_binding · ρ_i / ρ_binding. Не использовать double для денег (§9).
  static Decimal div(const Decimal& a, const Decimal& b, int32_t result_scale);

  // min(a,b) with same scale alignment
  /// Возвращает меньшее из двух чисел с учётом разного scale.
  /// Используется для clamp executed_qty не выше remaining_qty в matching/ledger.
  static Decimal min(const Decimal& a, const Decimal& b);

  // max(a,b)
  /// Возвращает большее из двух чисел. Используется реже — например, для
  /// floor/ceiling вычислений в risk checks.
  static Decimal max(const Decimal& a, const Decimal& b);

  // compare: -1,0,1
  /// Three-way compare: -1 если a < b, 0 если a == b, +1 если a > b.
  /// Учитывает разные scale через внутренний align. Канонический инструмент
  /// для условий if (Decimal::cmp(a, b) >= 0) — избегаем рантайм-сравнения
  /// двух разных representations напрямую через operator<.
  static int cmp(const Decimal& a, const Decimal& b);

  // zero: Decimal{0, 0}
  /// Каноническая константа нуля. Заметьте: Decimal{0,5} тоже равен 0, но
  /// canonical form — {0,0}, чтобы избежать ненужного scale в логах/SQL.
  static Decimal zero();

  // double(units * 10^(-scale))
  /// Cast к double. Помечен explicit — нельзя случайно "забыть" и передать
  /// Decimal туда, где ждут double (например, в floating-point метрику).
  /// Используется ТОЛЬКО для diagnostics/metrics/research (см. CLAUDE.md §9).
  /// Никогда не должен попадать в ledger settlement или persistence.
  explicit operator double() const;
};

}  // namespace cex::common
