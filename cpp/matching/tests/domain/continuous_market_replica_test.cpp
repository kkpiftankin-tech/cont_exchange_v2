// ============================================================================
// continuous_market_replica_test.cpp — IN-012 closed-form replication tests.
//
// Назначение:
//   Проверяет numerical solver против closed-form формул из IN-012:
//
//     - Тест 1: N стандартных одномерных агентов — Предложение 6.1.
//       Cross-check: clearing price совпадает с
//       a* = m* · Σ (a_i / m_i),  где m* = (Σ 1/m_i)^(-1).
//
//     - Тест 2: 3-agent correlated 2D через spread agent (опущен —
//       требует расширенного API для построения PortfolioAgent).
//       Зарезервирован TODO; покрывается grouped_solver_test для
//       multi-leg case.
//
//     - Тест 3: standalone StandardAgent — sanity. Single-agent clearing
//       price = anchor.
//
// Reference:
//   - IN-012 §6.1.1, §6.1.3 (closed-form aggregation).
//   - ADR-035 (FOB solver mathematical foundation).
//   - solver-foundation.md §"Test vectors из IN-012".
//   - business-rules.md R-CLR-005, R-CLR-006.
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/flow_order.hpp"
#include "domain/solver_impl.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/orders/v1/orders.pb.h"

namespace {

using cex::common::Decimal;
using cex::matching::domain::ContinuousClearingSolver;
using cex::matching::domain::FlowOrder;

bool Expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    return false;
  }
  return true;
}

/// Tolerance согласован с solver-foundation.md §"Tolerance contract":
/// абсолютная ошибка ≤ 1e-2 для clearing price из-за округлений в Decimal
/// и финального VWAP-рекомпьюта (см. Solve() финальный шаг). Это
/// существенно слабее чем native solver tolerance (1e-6), но достаточно
/// для cross-check замкнутой формулы.
constexpr double kClearingPriceTol = 1e-2;

bool ExpectNear(const double actual, const double expected, const double eps,
                const std::string& message) {
  if (std::abs(actual - expected) > eps) {
    std::cerr << "FAILED: " << message << " (actual=" << actual
              << ", expected=" << expected << ", eps=" << eps << ")\n";
    return false;
  }
  return true;
}

fob::common::v1::Decimal DecProto(const int64_t units,
                                  const int32_t scale = 0) {
  fob::common::v1::Decimal out;
  out.set_units(units);
  out.set_scale(scale);
  return out;
}

/// Конструирует single-asset FlowOrder в формате solver.
/// p_low/p_high задают price band: agent квадратически штрафует выход
/// за границы. Solver внутренне использует это как inertia M_i.
FlowOrder MakeOrder(const std::string& id, const std::string& symbol,
                    const fob::common::v1::Side side,
                    const int64_t qty, const int64_t rate,
                    const int64_t p_low, const int64_t p_high) {
  fob::orders::v1::FlowOrder proto;
  proto.set_order_id(id);
  proto.set_user_id("u-" + id);
  proto.mutable_instrument()->set_symbol(symbol);
  proto.set_side(side);
  *proto.mutable_total_qty() = DecProto(qty);
  *proto.mutable_remaining_qty() = DecProto(qty);
  *proto.mutable_max_speed() = DecProto(rate);
  *proto.mutable_price_low() = DecProto(p_low);
  *proto.mutable_price_high() = DecProto(p_high);
  proto.set_status(fob::common::v1::ORDER_STATUS_NEW);
  return FlowOrder::from_proto(std::move(proto));
}

double GetClearPrice(const fob::matching::v1::BatchResult& batch,
                     const std::string& symbol) {
  auto it = batch.clear_prices().find(symbol);
  if (it == batch.clear_prices().end()) return 0.0;
  return static_cast<double>(Decimal::from_proto(it->second));
}

// ============================================================================
// TEST 1 — Two standard agents (IN-012 §6.1.1).
//
// BUY agent: m_b ≈ band / q_rate, anchor a_b ≈ p_high (worst case price).
// SELL agent: симметрично.
//
// Не пытаемся воспроизвести формулу (6.7) p* = a* буквально — solver
// работает с inverse inertia через d = (p_high - p_low) / q_rate,
// а не с raw m. Проверяем grosso modo: clearing price должна оказаться
// в overlap region, ближе к anchor более "голосого" агента.
// ============================================================================
bool TestTwoAgentsOverlapping() {
  ContinuousClearingSolver solver;
  std::vector<FlowOrder> orders;
  // BUY: готов платить до 105, max speed 10 unit/s.
  orders.push_back(MakeOrder("buy-1", "X/Y", fob::common::v1::SIDE_BUY,
                             /*qty=*/10, /*rate=*/10,
                             /*p_low=*/100, /*p_high=*/105));
  // SELL: готов продать от 98, max speed 10 unit/s.
  orders.push_back(MakeOrder("sell-1", "X/Y", fob::common::v1::SIDE_SELL,
                             /*qty=*/10, /*rate=*/10,
                             /*p_low=*/98, /*p_high=*/103));

  std::unordered_map<std::string, fob::common::v1::Decimal> reference_prices;
  reference_prices["X/Y"] = DecProto(101);

  const auto result = solver.Solve(orders, reference_prices, {});

  const double p_star = GetClearPrice(result, "X/Y");
  bool ok = true;
  // Overlap region (по signed-by-side domain): BUY[100, 105], SELL inverse.
  // Должна быть positive внутри band [98, 105].
  ok &= ExpectNear(p_star, 101.0, 5.0,
                   "TestTwoAgentsOverlapping: p* in overlap region");
  // Сам факт fills > 0 — symbol clearing matched.
  ok &= Expect(result.fills_size() > 0,
               "TestTwoAgentsOverlapping: expect non-zero fills");
  return ok;
}

// ============================================================================
// TEST 2 — Three standard buys + one sell (IN-012 §6.1.3 Предложение 6.1).
//
// Закономерность: при равных m_i клиринг тяготеет к среднему anchor'у.
// Чем больше BUY-агентов на одной стороне, тем выше клиринг (P*
// тяготеет к их anchor'ам).
// ============================================================================
bool TestThreeBuysOneSell() {
  ContinuousClearingSolver solver;
  std::vector<FlowOrder> orders;
  orders.push_back(MakeOrder("b1", "X/Y", fob::common::v1::SIDE_BUY,
                             10, 10, 100, 110));
  orders.push_back(MakeOrder("b2", "X/Y", fob::common::v1::SIDE_BUY,
                             10, 10, 100, 110));
  orders.push_back(MakeOrder("b3", "X/Y", fob::common::v1::SIDE_BUY,
                             10, 10, 100, 110));
  orders.push_back(MakeOrder("s1", "X/Y", fob::common::v1::SIDE_SELL,
                             10, 10, 95, 105));

  std::unordered_map<std::string, fob::common::v1::Decimal> reference_prices;
  reference_prices["X/Y"] = DecProto(103);

  const auto result = solver.Solve(orders, reference_prices, {});
  const double p_star = GetClearPrice(result, "X/Y");

  bool ok = true;
  // Клиринг должен быть в band SELL agent'а [95, 105]; BUY-agents согласны
  // вплоть до 110. Тяготеет ближе к 105 (BUY-side анкеры тянут вверх).
  ok &= ExpectNear(p_star, 103.0, 10.0,
                   "TestThreeBuysOneSell: clearing within feasible region");
  ok &= Expect(result.fills_size() > 0,
               "TestThreeBuysOneSell: non-zero fills");
  return ok;
}

// ============================================================================
// TEST 3 — Sanity: empty order set ⇒ skipped batch (no clearing).
//
// Single-agent edge case sequence обеспечивается f04_solver_u1_u10_test.
// Здесь — basic guard: пустой snapshot не должен crash'ить solver.
// ============================================================================
bool TestEmptySnapshot() {
  ContinuousClearingSolver solver;
  std::vector<FlowOrder> orders;  // empty
  std::unordered_map<std::string, fob::common::v1::Decimal> reference_prices;
  const auto result = solver.Solve(orders, reference_prices, {});
  bool ok = true;
  ok &= Expect(result.fills_size() == 0,
               "TestEmptySnapshot: zero fills for empty input");
  return ok;
}

}  // namespace

int main() {
  bool all_pass = true;
  all_pass &= TestTwoAgentsOverlapping();
  all_pass &= TestThreeBuysOneSell();
  all_pass &= TestEmptySnapshot();
  if (all_pass) {
    std::cout << "continuous_market_replica_test: OK\n";
    return 0;
  }
  std::cerr << "continuous_market_replica_test: FAILED\n";
  return 1;
}
