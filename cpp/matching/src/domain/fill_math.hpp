#pragma once
// ============================================================================
// fill_math.hpp — чистая доменная функция накопления fill-объёма с clamping
// до q_max и определением итогового статуса FlowOrder.
//
// Физический смысл:
//   FlowOrder задаёт максимально допустимый исполняемый объём q_max (в base
//   units, "portfolio units" в терминах CSLO — это базовый актив, например BTC).
//   На каждом батче matching производит исполнение, наполняющее filled_cum.
//   Эта функция отвечает на единственный вопрос:
//     "Учитывая предыдущий cumulative fill и delta этого батча, какой будет
//      новый cumulative fill и статус заявки?"
//
//   Физические ограничения:
//   * delta >= 0 (нельзя "разисполнить" — отмена идёт через cancel-flow);
//   * filled_cum <= q_max (clamping; защита от arithmetic drift из solver);
//   * filled_cum == q_max → FlowOrderStatus::kFilled;
//   * 0 < filled_cum < q_max → FlowOrderStatus::kPartiallyFilled.
//
// Почему inline в .hpp:
//   1. Однократно используется в run_batch_uc.cpp; нет смысла в .cpp.
//   2. Pure-функция без side-effects, идеальна для unit-тестов.
//   3. inline гарантирует ODR-safe inclusion в нескольких translation units.
//
// Связь с CLAUDE.md:
//   §8.2 FlowOrder инварианты (3): remaining_qty <= total_qty;
//   §15 matching invariants — Fill не должен превышать remaining_qty заявки.
// ============================================================================

#include <stdexcept>   // std::invalid_argument для контракта delta>=0

#include "cex/common/decimal.hpp"     // денежный тип (см. decimal.hpp)
#include "domain/flow_order.hpp"      // FlowOrderStatus enum

namespace cex::matching::domain {

/// Результат расчёта: новое значение filled_cum и новый статус заявки.
/// Структура aggregate-initializable; используется как value-object.
struct FilledVolumeUpdate {
  // Cumulative fill in portfolio units.
  cex::common::Decimal new_filled_cum{};       ///< Новое cumulative-исполненное (после clamp).
  FlowOrderStatus new_status{FlowOrderStatus::kPartiallyFilled};
                                                ///< Default = частично — финальный фикс ниже.
};

/// Обновляет cumulative fill и вычисляет статус FlowOrder.
///
/// Параметры:
///   q_max_portfolio_units                   — верхняя граница объёма заявки.
///   previous_filled_cum_portfolio_units     — накопленный объём до этого батча.
///   executed_qty_delta_portfolio_units      — сколько добавил текущий батч.
///
/// Возвращает: { new_filled_cum, new_status }.
/// Бросает std::invalid_argument если delta < 0 (нарушение инварианта).
///
/// Особый случай: если unclamped > q_max (solver выдал чуть больше из-за
/// rounding), clamp к q_max и статус = kFilled — это защита от arithmetic
/// drift, не реальное "переисполнение".
inline FilledVolumeUpdate ComputeFilledVolumeUpdate(
    const cex::common::Decimal& q_max_portfolio_units,
    const cex::common::Decimal& previous_filled_cum_portfolio_units,
    const cex::common::Decimal& executed_qty_delta_portfolio_units) {
  // Нулевая константа в том же scale что и delta — нужна для корректного
  // Decimal::cmp без implicit-rescale внутри сравнения.
  const cex::common::Decimal zero{0, executed_qty_delta_portfolio_units.scale};

  // Инвариант: delta >= 0. Отрицательное исполнение запрещено (см. CLAUDE.md §15).
  // cmp возвращает -1/0/+1; <0 значит delta < zero.
  if (cex::common::Decimal::cmp(executed_qty_delta_portfolio_units, zero) < 0) {
    throw std::invalid_argument("executed_qty_delta_portfolio_units must be non-negative");
  }

  // Сначала суммируем без clamp — это "идеальный" новый cumulative fill.
  const auto unclamped = cex::common::Decimal::add(
      previous_filled_cum_portfolio_units,
      executed_qty_delta_portfolio_units);

  // Clamp к q_max — защищаемся от случая, когда unclamped чуть выше q_max
  // из-за floating-point округлений в solver или сложений со scale-конфликтами.
  const auto new_filled_cum = cex::common::Decimal::min(unclamped, q_max_portfolio_units);

  // Сборка результата. Aggregate-init не используем чтобы явно показать
  // условную ветку для статуса.
  FilledVolumeUpdate update;
  update.new_filled_cum = new_filled_cum;
  // Если новый cumulative >= q_max → заявка полностью исполнена.
  // Иначе остаётся в kPartiallyFilled (даже если delta=0 и previous=0 —
  // вызывающий код должен ловить такой случай отдельно).
  update.new_status = cex::common::Decimal::cmp(new_filled_cum, q_max_portfolio_units) >= 0
                          ? FlowOrderStatus::kFilled
                          : FlowOrderStatus::kPartiallyFilled;
  return update;
}

}  // namespace cex::matching::domain
