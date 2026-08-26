// ============================================================================
// vector_clearing_result_mapper_test.cpp — F-05A (T-F05A-305, 1a). Hand-rolled.
// VectorClearingOutcome → proto VectorClearingResult (диагностика, без денег).
// ============================================================================

#include <iostream>
#include <string>

#include "app/vector_clearing_use_case.hpp"
#include "transport/mappers/vector_clearing_result.hpp"

namespace {

using cex::common::Decimal;
namespace app = cex::matching::app;
namespace dom = cex::matching::domain;
namespace tr = cex::matching::transport;
namespace mv1 = fob::marketdata::v1;

int g_failures = 0;
bool expect(bool cond, const std::string& msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; ++g_failures; }
  return cond;
}

void TestConverged() {
  app::VectorClearingOutcome o;
  o.solve.x = {Decimal{2000000000000LL, 12}, Decimal{1000000000000LL, 12}};
  o.solve.residual = {0.0, 0.0};
  o.solve.residual_norm = 0.0;
  o.solve.iterations = 5;
  o.solve.status = dom::VectorSolveStatus::kConverged;
  o.surplus.action = dom::SurplusAction::kProceedNoSurplus;

  mv1::VectorClearingResult r = tr::ToVectorClearingResult(o, "b1");
  expect(r.batch_id() == "b1", "batch_id");
  expect(r.solver_status() == mv1::VECTOR_SOLVER_STATUS_CONVERGED, "status CONVERGED");
  expect(r.x_size() == 2 && r.x(0).units() == 2000000000000LL, "x mapped");
  expect(r.residual_size() == 2, "residual per asset");
  expect(r.diagnostics().residual_norm() == 0.0 && r.diagnostics().iterations() == 5,
         "diagnostics");
  expect(r.surplus_size() == 0, "no surplus when balanced");
}

void TestSurplusExchangePnl() {
  app::VectorClearingOutcome o;
  o.solve.x = {Decimal{5000000000000LL, 12}};
  o.solve.residual = {0.5};
  o.solve.residual_norm = 0.5;
  o.solve.status = dom::VectorSolveStatus::kDegraded;
  o.surplus.action = dom::SurplusAction::kAllocateExchangePnl;
  o.surplus.surplus_by_asset = {Decimal{500000000000LL, 12}};  // 0.5

  mv1::VectorClearingResult r = tr::ToVectorClearingResult(o, "b2");
  expect(r.solver_status() == mv1::VECTOR_SOLVER_STATUS_DEGRADED, "status DEGRADED");
  expect(r.surplus_size() == 1, "surplus emitted");
  expect(r.surplus(0).allocation_policy() ==
             mv1::SURPLUS_ALLOCATION_POLICY_EXCHANGE_PNL,
         "surplus policy EXCHANGE_PNL");
  expect(r.surplus(0).amount().units() == 500000000000LL, "surplus amount 0.5");
}

}  // namespace

int main() {
  TestConverged();
  TestSurplusExchangePnl();
  if (g_failures == 0) {
    std::cout << "vector_clearing_result_mapper_test: ALL PASSED\n";
    return 0;
  }
  std::cerr << "vector_clearing_result_mapper_test: " << g_failures << " FAILURE(S)\n";
  return 1;
}
