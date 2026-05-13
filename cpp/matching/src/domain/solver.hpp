#pragma once
// =============================================================================
// Контракт солвера непрерывного клиринга. Это тот самый "точечный шов" между
// application-слоем и доменом: подменяя реализацию IContinuousClearingSolver,
// можно перейти от MVP-симулятора к настоящей оптимизационной задаче без
// изменения остального кода сервиса.
//
// Ожидаемая модель:
//   * переменные решения: исполненные скорости x_i (или dq_i на интервал dt);
//   * ограничения: 0 <= x_i <= u_i (доступная скорость), бюджет/инвентарь и т.п.;
//   * цель: максимизация welfare / минимизация выпуклой стоимости + регуляризация.
//
// В MVP метод ничем не реализуется — солвер встроен прямо в matching_loop.cpp.
// =============================================================================

#include <vector>

#include "fob/orders/v1/orders.pb.h"
#include "fob/matching/v1/batch.pb.h"

namespace cex::matching::domain {

struct IContinuousClearingSolver {
  // Виртуальный деструктор обязателен, т.к. через интерфейс будут владеть наследниками.
  virtual ~IContinuousClearingSolver() = default;

  // Один шаг клиринга по списку активных flow-ордеров.
  // Должен вернуть полный BatchResult: fills, executed_rates, clear_prices,
  // order_updates и diagnostics.
  virtual fob::matching::v1::BatchResult Solve(
      const std::vector<fob::orders::v1::FlowOrder>& active_orders) = 0;
};

}  // namespace cex::matching::domain
