// ============================================================================
// vector_qp_solver_test.cpp — F-05A (T-F05A-302/303). Hand-rolled harness.
//
// Проверяет доменное ядро QP-солвера БЕЗ реального OSQP (backend — fake):
//   1) AssembleProblem: standard-form по ADR-048 (P=D, q=−pH, A=[W;I], l, u);
//   2) residual r=Wx и residual_norm;
//   3) статусы converged / degraded / failed;
//   4) квантование x → Decimal (scale=12, §9);
//   5) детерминизм (тот же вход → тот же результат);
//   6) пустая задача.
// ============================================================================

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <eigen3/Eigen/Dense>

#include "domain/vector_qp_solver.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::matching::domain;

int g_failures = 0;

bool expect(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "FAILED: " << msg << '\n';
    ++g_failures;
  }
  return cond;
}

bool approx(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

// Fake backend: возвращает заранее заданные x и статус, игнорируя задачу.
// Позволяет тестировать сборку/residual/квантование независимо от OSQP.
struct FakeBackend final : public d::IQpBackend {
  Eigen::VectorXd x_out;
  d::QpBackendStatus status_out{d::QpBackendStatus::kSolved};
  std::uint32_t iters{7};
  int calls{0};

  d::QpSolution Solve(const d::QpProblem&, const d::QpParams&) override {
    ++calls;
    d::QpSolution s;
    s.x = x_out;
    s.status = status_out;
    s.iterations = iters;
    return s;
  }
};

d::VectorSegment MakeSeg(const std::string& id, std::vector<double> w, double d_hl,
                         double q_max) {
  d::VectorSegment s;
  s.segment_id = id;
  s.source_order_id = "ord-" + id;
  s.w = std::move(w);
  s.d_hl = d_hl;
  s.q_max = q_max;
  return s;
}

// --- 1) AssembleProblem: standard-form по ADR-048 -------------------------
void TestAssemble() {
  // N=2 актива, I=2 сегмента.
  std::vector<d::VectorSegment> segs = {
      MakeSeg("s0", {1.0, -2.0}, /*d_hl=*/4.0, /*q_max=*/8.0),
      MakeSeg("s1", {-1.0, 3.0}, /*d_hl=*/6.0, /*q_max=*/3.0),
  };
  d::QpProblem p = d::VectorQpSolver::AssembleProblem(segs, /*num_assets=*/2);

  expect(p.num_assets == 2 && p.num_segments == 2, "assemble: dims N=2 I=2");

  // P = diag(dHL/q) = diag(4/8, 6/3) = diag(0.5, 2.0)
  expect(p.P.rows() == 2 && p.P.cols() == 2, "assemble: P 2x2");
  expect(approx(p.P(0, 0), 0.5) && approx(p.P(1, 1), 2.0), "assemble: P diag = dHL/q");
  expect(approx(p.P(0, 1), 0.0) && approx(p.P(1, 0), 0.0), "assemble: P off-diag 0");

  // q = -pH = -[4,6]
  expect(approx(p.q(0), -4.0) && approx(p.q(1), -6.0), "assemble: q = -pH");

  // A = [W; I] = [[1,-1],[-2,3],[1,0],[0,1]]  (4x2)
  expect(p.A.rows() == 4 && p.A.cols() == 2, "assemble: A (N+I)xI = 4x2");
  expect(approx(p.A(0, 0), 1.0) && approx(p.A(0, 1), -1.0), "assemble: A row0 = W col contrib");
  expect(approx(p.A(1, 0), -2.0) && approx(p.A(1, 1), 3.0), "assemble: A row1 = W");
  expect(approx(p.A(2, 0), 1.0) && approx(p.A(2, 1), 0.0), "assemble: A identity row I0");
  expect(approx(p.A(3, 0), 0.0) && approx(p.A(3, 1), 1.0), "assemble: A identity row I1");

  // l = [0,0,0,0]; u = [0,0, q_box] = [0,0,8,3]
  expect(approx(p.l(0), 0.0) && approx(p.l(1), 0.0) && approx(p.l(2), 0.0) &&
             approx(p.l(3), 0.0),
         "assemble: l all zero");
  expect(approx(p.u(0), 0.0) && approx(p.u(1), 0.0), "assemble: u top N = 0 (Wx=0)");
  expect(approx(p.u(2), 8.0) && approx(p.u(3), 3.0), "assemble: u bottom = q_box");
}

// --- 2/3/4) Solve: converged + residual + квантование ---------------------
void TestSolveConverged() {
  // col1 = -2*col0 ⇒ x=[2,1] даёт Wx=0.  col0=[1,-2], col1=[-2,4].
  std::vector<d::VectorSegment> segs = {
      MakeSeg("s0", {1.0, -2.0}, 4.0, 8.0),
      MakeSeg("s1", {-2.0, 4.0}, 6.0, 8.0),
  };
  FakeBackend be;
  be.x_out = Eigen::Vector2d(2.0, 1.0);
  be.status_out = d::QpBackendStatus::kSolved;

  d::VectorQpSolver solver(be, d::QpParams{}, /*tol=*/1e-9);
  d::VectorClearingResult r = solver.Solve(segs, 2);

  expect(r.status == d::VectorSolveStatus::kConverged, "solve: Wx=0 ⇒ converged");
  expect(approx(r.residual_norm, 0.0), "solve: residual_norm ≈ 0");
  expect(r.residual.size() == 2 && approx(r.residual[0], 0.0) && approx(r.residual[1], 0.0),
         "solve: residual vector = 0");
  // Квантование x=[2,1] при scale=12
  expect(r.x.size() == 2, "solve: x has 2 entries");
  expect(r.x[0].scale == 12 && r.x[0].units == 2000000000000LL, "solve: x0 = Decimal 2.0 @scale12");
  expect(r.x[1].units == 1000000000000LL, "solve: x1 = Decimal 1.0 @scale12");
  expect(r.iterations == 7, "solve: iterations propagated");
}

// --- 3) Degraded: ненулевой остаток --------------------------------------
void TestSolveDegraded() {
  std::vector<d::VectorSegment> segs = {
      MakeSeg("s0", {1.0, -2.0}, 4.0, 8.0),
      MakeSeg("s1", {-2.0, 4.0}, 6.0, 8.0),
  };
  FakeBackend be;
  be.x_out = Eigen::Vector2d(2.0, 2.0);  // Wx = [1*2-2*2, -2*2+4*2] = [-2, 4] ≠ 0
  be.status_out = d::QpBackendStatus::kSolved;

  d::VectorQpSolver solver(be, d::QpParams{}, 1e-9);
  d::VectorClearingResult r = solver.Solve(segs, 2);

  expect(r.status == d::VectorSolveStatus::kDegraded, "solve: Wx≠0 ⇒ degraded");
  expect(approx(r.residual[0], -2.0) && approx(r.residual[1], 4.0), "solve: residual = Wx");
  expect(approx(r.residual_norm, std::sqrt(20.0)), "solve: residual_norm = ||Wx||");
}

// --- 4b) Квантование дробного и малого отрицательного ---------------------
void TestQuantize() {
  std::vector<d::VectorSegment> segs = {MakeSeg("s0", {0.0}, 1.0, 1.0)};
  FakeBackend be;
  Eigen::VectorXd x(1);
  x(0) = 0.5;
  be.x_out = x;
  be.status_out = d::QpBackendStatus::kSolved;

  d::VectorQpSolver solver(be, d::QpParams{}, 1e-9);
  d::VectorClearingResult r = solver.Solve(segs, 1);
  expect(r.x[0].units == 500000000000LL, "quantize: 0.5 → 500000000000 @scale12");

  // Крошечное отрицательное (солверный шум) → 0.
  Eigen::VectorXd xn(1);
  xn(0) = -1e-15;
  be.x_out = xn;
  d::VectorClearingResult r2 = solver.Solve(segs, 1);
  expect(r2.x[0].units == 0, "quantize: -1e-15 → 0");
}

// --- 5) Failed: infeasible backend ---------------------------------------
void TestSolveFailed() {
  std::vector<d::VectorSegment> segs = {MakeSeg("s0", {1.0}, 1.0, 1.0)};
  FakeBackend be;
  be.status_out = d::QpBackendStatus::kPrimalInfeasible;

  d::VectorQpSolver solver(be, d::QpParams{}, 1e-9);
  d::VectorClearingResult r = solver.Solve(segs, 1);
  expect(r.status == d::VectorSolveStatus::kFailed, "solve: infeasible ⇒ failed");
  expect(r.x.empty(), "solve: failed ⇒ no x (no money postings)");
}

// --- 6) Детерминизм -------------------------------------------------------
void TestDeterminism() {
  std::vector<d::VectorSegment> segs = {
      MakeSeg("s0", {1.0, -2.0}, 4.0, 8.0),
      MakeSeg("s1", {-2.0, 4.0}, 6.0, 8.0),
  };
  FakeBackend be;
  be.x_out = Eigen::Vector2d(2.0, 1.0);
  d::VectorQpSolver solver(be, d::QpParams{}, 1e-9);

  d::VectorClearingResult a = solver.Solve(segs, 2);
  d::VectorClearingResult b = solver.Solve(segs, 2);
  bool same = a.status == b.status && a.x.size() == b.x.size() &&
              approx(a.residual_norm, b.residual_norm, 0.0);
  for (std::size_t i = 0; same && i < a.x.size(); ++i) {
    same = a.x[i].units == b.x[i].units && a.x[i].scale == b.x[i].scale;
  }
  expect(same, "determinism: same input ⇒ identical result (AC-F05A-011)");
}

// --- 7) Пустая задача -----------------------------------------------------
void TestEmpty() {
  FakeBackend be;
  d::VectorQpSolver solver(be, d::QpParams{}, 1e-9);
  d::VectorClearingResult r = solver.Solve({}, 2);
  expect(r.status == d::VectorSolveStatus::kConverged, "empty: converged trivially");
  expect(r.x.empty() && be.calls == 0, "empty: backend not called");
}

}  // namespace

int main() {
  TestAssemble();
  TestSolveConverged();
  TestSolveDegraded();
  TestQuantize();
  TestSolveFailed();
  TestDeterminism();
  TestEmpty();

  if (g_failures == 0) {
    std::cout << "vector_qp_solver_test: ALL PASSED\n";
    return 0;
  }
  std::cerr << "vector_qp_solver_test: " << g_failures << " FAILURE(S)\n";
  return 1;
}
