#pragma once
// ============================================================================
// vector_clearing_use_case.hpp — F-05A (T-F05A-305, safe slice). Matching app.
//
// Оркестрация векторного клиринга БЕЗ эмиссии денег:
//   proto VectorClearingInput  → domain::VectorSegment[]  (mapping)
//                              → VectorQpSolver(OsqpBackend)  (solve, Wx=0)
//                              → surplus_policy::DecideSurplus (по остатку)
//   → VectorClearingOutcome (x в Decimal, residual, status, surplus-решение).
//
// НЕ эмитит ExecutionGroup/FillEvent и не делает ledger-проводок — money-path
// (T-F05A-305 full) отдельным явным шагом. Здесь только solve + диагностика.
// ============================================================================

#include <vector>

#include "fob/marketdata/v1/vector_liquidity.pb.h"

#include "domain/surplus_policy.hpp"
#include "domain/vector_qp_solver.hpp"

namespace cex::matching::app {

struct VectorClearingOutcome {
  domain::VectorClearingResult solve;   ///< x (Decimal), residual, status
  domain::SurplusDecision surplus;      ///< решение по остатку (ADR-047)
  int num_assets{0};
  int num_segments{0};
};

class VectorClearingUseCase {
 public:
  VectorClearingUseCase(domain::IVectorClearingSolver& solver,
                        domain::SurplusPolicy policy =
                            domain::SurplusPolicy::kRejectIfResidual,
                        double residual_tolerance = 1e-9,
                        std::int32_t decimal_scale = 12)
      : solver_(solver),
        policy_(policy),
        residual_tolerance_(residual_tolerance),
        decimal_scale_(decimal_scale) {}

  /// Решить клиринг для входа. Чистая оркестрация (детерминирована при
  /// детерминированном backend). Денежных сайд-эффектов нет.
  VectorClearingOutcome Clear(
      const fob::marketdata::v1::VectorClearingInput& input) const;

  /// proto-сегменты → domain::VectorSegment (w/d_hl/q_max: Decimal→double).
  /// Публично для юнит-тестов.
  static std::vector<domain::VectorSegment> MapSegments(
      const fob::marketdata::v1::VectorClearingInput& input);

 private:
  domain::IVectorClearingSolver& solver_;
  domain::SurplusPolicy policy_;
  double residual_tolerance_;
  std::int32_t decimal_scale_;
};

}  // namespace cex::matching::app
