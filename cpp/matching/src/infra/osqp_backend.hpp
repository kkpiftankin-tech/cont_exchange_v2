#pragma once
// ============================================================================
// osqp_backend.hpp — F-05A (T-F05A-301). Matching infra.
//
// Реализация порта IQpBackend поверх OSQP (ADR-048: OSQP решает QP; Eigen лишь
// собирает W/D в доменном слое). Инфраструктурный адаптер: конвертирует плотные
// Eigen-матрицы стандартной формы (P, q, A, l, u) в CSC и вызывает OSQP.
//
// Детерминизм (ADR-048 / AC-F05A-011): OSQP настраивается с adaptive_rho=0,
// фиксированным max_iter/tolerances, без warm-start и без таймеров — результат
// воспроизводим на тех же входах. Никаких денежных величин здесь нет (backend
// работает в double; квантование x→Decimal — в доменном VectorQpSolver, §9).
// ============================================================================

#include "domain/vector_qp_solver.hpp"

namespace cex::matching::infra {

class OsqpBackend final : public cex::matching::domain::IQpBackend {
 public:
  OsqpBackend() = default;

  /// Решить QP (стандартная форма OSQP) детерминированно. Потокобезопасен:
  /// не хранит состояния между вызовами (каждый Solve — свой OSQP workspace).
  cex::matching::domain::QpSolution Solve(
      const cex::matching::domain::QpProblem& problem,
      const cex::matching::domain::QpParams& params) override;
};

}  // namespace cex::matching::infra
