// ============================================================================
// vector_clearing_result.cpp — F-05A (T-F05A-305, 1a). См. заголовок в .hpp.
// ============================================================================

#include "transport/mappers/vector_clearing_result.hpp"

#include <cmath>  // std::isfinite, std::llround, std::pow

#include "cex/common/decimal.hpp"

namespace cex::matching::transport {

namespace mv1 = fob::marketdata::v1;

namespace {

fob::common::v1::Decimal QuantizeToProto(double value, std::int32_t scale) {
  cex::common::Decimal d{0, scale};
  if (std::isfinite(value)) {
    const double factor = std::pow(10.0, static_cast<double>(scale));
    d.units = static_cast<std::int64_t>(std::llround(value * factor));
  }
  return d.to_proto();
}

mv1::VectorSolverStatus MapStatus(domain::VectorSolveStatus s) {
  switch (s) {
    case domain::VectorSolveStatus::kConverged:
      return mv1::VECTOR_SOLVER_STATUS_CONVERGED;
    case domain::VectorSolveStatus::kDegraded:
      return mv1::VECTOR_SOLVER_STATUS_DEGRADED;
    case domain::VectorSolveStatus::kFailed:
    default:
      return mv1::VECTOR_SOLVER_STATUS_FAILED;
  }
}

mv1::SurplusAllocationPolicy MapPolicy(domain::SurplusAction action) {
  switch (action) {
    case domain::SurplusAction::kReject:
      return mv1::SURPLUS_ALLOCATION_POLICY_REJECT_IF_RESIDUAL;
    case domain::SurplusAction::kAllocateExchangePnl:
      return mv1::SURPLUS_ALLOCATION_POLICY_EXCHANGE_PNL;
    case domain::SurplusAction::kAllocateSurplusAsset:
      return mv1::SURPLUS_ALLOCATION_POLICY_SURPLUS_ASSET;
    case domain::SurplusAction::kAllocateMm:
      return mv1::SURPLUS_ALLOCATION_POLICY_MM_LAST_RESORT;
    case domain::SurplusAction::kProceedNoSurplus:
    default:
      return mv1::SURPLUS_ALLOCATION_POLICY_UNSPECIFIED;
  }
}

}  // namespace

mv1::VectorClearingResult ToVectorClearingResult(
    const app::VectorClearingOutcome& outcome, const std::string& batch_id,
    std::int32_t decimal_scale) {
  mv1::VectorClearingResult res;
  res.set_batch_id(batch_id);
  res.set_solver_status(MapStatus(outcome.solve.status));

  // x (executed rate per segment) — уже Decimal.
  for (const auto& xi : outcome.solve.x) {
    *res.add_x() = xi.to_proto();
  }
  // residual (Wx per asset) — double → Decimal (диагностика).
  for (double r : outcome.solve.residual) {
    *res.add_residual() = QuantizeToProto(r, decimal_scale);
  }

  // diagnostics.
  auto* diag = res.mutable_diagnostics();
  diag->set_residual_norm(outcome.solve.residual_norm);
  diag->set_iterations(outcome.solve.iterations);

  // surplus (по ADR-047): amounts по активам, если политика аллоцирует остаток.
  const mv1::SurplusAllocationPolicy policy = MapPolicy(outcome.surplus.action);
  for (const auto& amt : outcome.surplus.surplus_by_asset) {
    auto* si = res.add_surplus();
    *si->mutable_amount() = amt.to_proto();
    si->set_allocation_policy(policy);
  }

  return res;
}

}  // namespace cex::matching::transport
