// ============================================================================
// vector_clearing_use_case_test.cpp — F-05A (T-F05A-305, safe slice).
// Hand-rolled harness. proto VectorClearingInput → map → OSQP solve → surplus.
//   1) MapSegments: proto Decimal w/d_hl/q_max → domain double;
//   2) end-to-end: feasible (Wx=0) ⇒ converged + surplus proceed (no money).
// ============================================================================

#include <iostream>
#include <string>

#include "fob/marketdata/v1/vector_liquidity.pb.h"

#include "app/vector_clearing_use_case.hpp"
#include "domain/surplus_policy.hpp"
#include "domain/vector_qp_solver.hpp"
#include "infra/osqp_backend.hpp"

namespace {

namespace app = cex::matching::app;
namespace dom = cex::matching::domain;
namespace mv1 = fob::marketdata::v1;

int g_failures = 0;
bool expect(bool cond, const std::string& msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; ++g_failures; }
  return cond;
}

void SetDec(fob::common::v1::Decimal* d, std::int64_t units, std::int32_t scale = 0) {
  d->set_units(units);
  d->set_scale(scale);
}

mv1::VectorClearingInput MakeInput() {
  mv1::VectorClearingInput in;
  in.set_batch_id("b1");
  in.mutable_basis()->set_num_assets(2);
  // seg0: w=[1,-2], d_hl=4, q_max=8
  auto* s0 = in.add_segments();
  s0->set_segment_id("s0");
  SetDec(s0->add_w(), 1);
  SetDec(s0->add_w(), -2);
  SetDec(s0->mutable_d_hl(), 4);
  SetDec(s0->mutable_q_max(), 8);
  // seg1: w=[-2,4], d_hl=6, q_max=8
  auto* s1 = in.add_segments();
  s1->set_segment_id("s1");
  SetDec(s1->add_w(), -2);
  SetDec(s1->add_w(), 4);
  SetDec(s1->mutable_d_hl(), 6);
  SetDec(s1->mutable_q_max(), 8);
  return in;
}

void TestMapSegments() {
  auto segs = app::VectorClearingUseCase::MapSegments(MakeInput());
  expect(segs.size() == 2, "map: 2 segments");
  expect(segs[0].segment_id == "s0" && segs[0].w.size() == 2, "map: seg0 id+w");
  expect(segs[0].w[0] == 1.0 && segs[0].w[1] == -2.0, "map: seg0 w = [1,-2]");
  expect(segs[0].d_hl == 4.0 && segs[0].q_max == 8.0, "map: seg0 d_hl/q_max");
  expect(segs[1].w[0] == -2.0 && segs[1].w[1] == 4.0, "map: seg1 w = [-2,4]");
}

void TestClearEndToEnd() {
  cex::matching::infra::OsqpBackend be;
  dom::VectorQpSolver solver(be, dom::QpParams{}, /*tol=*/1e-6);
  app::VectorClearingUseCase uc(solver, dom::SurplusPolicy::kRejectIfResidual, 1e-6);

  app::VectorClearingOutcome out = uc.Clear(MakeInput());

  expect(out.num_assets == 2 && out.num_segments == 2, "clear: dims");
  expect(out.solve.status == dom::VectorSolveStatus::kConverged,
         "clear: feasible Wx=0 ⇒ converged");
  expect(out.solve.residual_norm <= 1e-6, "clear: residual ≈ 0");
  expect(out.solve.x.size() == 2, "clear: x has 2 entries");
  // x >= 0 (box), с точностью квантования
  expect(out.solve.x[0].units >= 0 && out.solve.x[1].units >= 0, "clear: x >= 0");
  // сбалансировано ⇒ surplus proceed, fills применились бы (но эмиссия — не здесь)
  expect(out.surplus.action == dom::SurplusAction::kProceedNoSurplus,
         "clear: no surplus (balanced)");
  expect(out.surplus.emit_fills, "clear: fills would apply");
}

}  // namespace

int main() {
  TestMapSegments();
  TestClearEndToEnd();
  if (g_failures == 0) {
    std::cout << "vector_clearing_use_case_test: ALL PASSED\n";
    return 0;
  }
  std::cerr << "vector_clearing_use_case_test: " << g_failures << " FAILURE(S)\n";
  return 1;
}
