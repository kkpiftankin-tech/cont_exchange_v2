// ============================================================================
// osqp_backend_test.cpp — F-05A (T-F05A-301). Hand-rolled harness.
//
// Проверяет реальный OSQP-backend на QP с известными решениями + end-to-end
// через VectorQpSolver (сборка → OSQP → residual → квантование Decimal).
//   1) внутренний минимум (box не активен): x* = pH/d;
//   2) активный box: x* = q (clamp);
//   3) равенство Wx=0: ограничение выполнено;
//   4) end-to-end VectorQpSolver(OsqpBackend): converged, residual ≈ 0.
// ============================================================================

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <eigen3/Eigen/Dense>

#include "domain/vector_qp_solver.hpp"
#include "infra/osqp_backend.hpp"

namespace {

namespace dm = cex::matching::domain;

int g_failures = 0;

bool expect(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "FAILED: " << msg << '\n';
    ++g_failures;
  }
  return cond;
}

bool approx(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

dm::QpProblem MakeProblem(Eigen::MatrixXd P, Eigen::VectorXd q, Eigen::MatrixXd A,
                          Eigen::VectorXd l, Eigen::VectorXd u) {
  dm::QpProblem p;
  p.P = std::move(P);
  p.q = std::move(q);
  p.A = std::move(A);
  p.l = std::move(l);
  p.u = std::move(u);
  p.num_segments = static_cast<int>(p.P.cols());
  p.num_assets = static_cast<int>(p.A.rows()) - p.num_segments;
  return p;
}

// --- 1) Внутренний минимум: min ½·2·x² − 4x s.t. 0≤x≤8 ⇒ x*=2 -------------
void TestInterior() {
  dm::QpProblem prob = MakeProblem(
      (Eigen::MatrixXd(1, 1) << 2.0).finished(),
      (Eigen::VectorXd(1) << -4.0).finished(),
      (Eigen::MatrixXd(1, 1) << 1.0).finished(),  // box row
      (Eigen::VectorXd(1) << 0.0).finished(),
      (Eigen::VectorXd(1) << 8.0).finished());
  cex::matching::infra::OsqpBackend be;
  dm::QpSolution s = be.Solve(prob, dm::QpParams{});
  expect(s.status == dm::QpBackendStatus::kSolved, "interior: solved");
  expect(approx(s.x(0), 2.0), "interior: x* = pH/d = 2");
}

// --- 2) Активный box: тот же QP, но u=1 ⇒ x*=1 ---------------------------
void TestBoxActive() {
  dm::QpProblem prob = MakeProblem(
      (Eigen::MatrixXd(1, 1) << 2.0).finished(),
      (Eigen::VectorXd(1) << -4.0).finished(),
      (Eigen::MatrixXd(1, 1) << 1.0).finished(),
      (Eigen::VectorXd(1) << 0.0).finished(),
      (Eigen::VectorXd(1) << 1.0).finished());
  cex::matching::infra::OsqpBackend be;
  dm::QpSolution s = be.Solve(prob, dm::QpParams{});
  expect(s.status == dm::QpBackendStatus::kSolved, "box: solved");
  expect(approx(s.x(0), 1.0), "box: x* clamped to q=1");
}

// --- 3) Равенство Wx=0: x0 = 2·x1 ---------------------------------------
void TestEquality() {
  // P=diag(0.5,2), q=[-4,-6]; A=[W;I]=[[1,-2],[1,0],[0,1]]; u=[0,8,8], l=0.
  Eigen::MatrixXd A(3, 2);
  A << 1.0, -2.0,  // W: x0 - 2 x1 = 0
       1.0, 0.0,   // I
       0.0, 1.0;
  dm::QpProblem prob = MakeProblem(
      (Eigen::MatrixXd(2, 2) << 0.5, 0.0, 0.0, 2.0).finished(),
      (Eigen::VectorXd(2) << -4.0, -6.0).finished(), A,
      (Eigen::VectorXd(3) << 0.0, 0.0, 0.0).finished(),
      (Eigen::VectorXd(3) << 0.0, 8.0, 8.0).finished());
  cex::matching::infra::OsqpBackend be;
  dm::QpSolution s = be.Solve(prob, dm::QpParams{});
  expect(s.status == dm::QpBackendStatus::kSolved, "equality: solved");
  expect(approx(s.x(0) - 2.0 * s.x(1), 0.0), "equality: Wx=0 (x0 = 2 x1)");
}

// --- 4) End-to-end: VectorQpSolver + OsqpBackend --------------------------
void TestEndToEnd() {
  std::vector<dm::VectorSegment> segs(2);
  segs[0].segment_id = "s0";
  segs[0].w = {1.0};   // W = [1, -2] (N=1)
  segs[0].d_hl = 4.0;
  segs[0].q_max = 8.0;
  segs[1].segment_id = "s1";
  segs[1].w = {-2.0};
  segs[1].d_hl = 6.0;
  segs[1].q_max = 8.0;

  cex::matching::infra::OsqpBackend be;
  dm::VectorQpSolver solver(be, dm::QpParams{}, /*tol=*/1e-4);
  dm::VectorClearingResult r = solver.Solve(segs, /*num_assets=*/1);

  expect(r.status == dm::VectorSolveStatus::kConverged,
         "e2e: Wx=0 feasible ⇒ converged");
  expect(r.residual_norm <= 1e-4, "e2e: residual_norm ≈ 0");
  expect(r.x.size() == 2, "e2e: x has 2 quantized entries");
  // x_i должны быть неотрицательны (box 0≤x) с точностью квантования.
  expect(r.x[0].units >= 0 && r.x[1].units >= 0, "e2e: x >= 0");
}

}  // namespace

int main() {
  TestInterior();
  TestBoxActive();
  TestEquality();
  TestEndToEnd();

  if (g_failures == 0) {
    std::cout << "osqp_backend_test: ALL PASSED\n";
    return 0;
  }
  std::cerr << "osqp_backend_test: " << g_failures << " FAILURE(S)\n";
  return 1;
}
