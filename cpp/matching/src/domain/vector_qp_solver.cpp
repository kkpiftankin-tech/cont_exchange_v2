// ============================================================================
// vector_qp_solver.cpp — F-05A (T-F05A-302 / T-F05A-303). См. заголовок в .hpp.
// Сборка standard-form (ADR-048) + вызов IQpBackend + residual/диагностика +
// квантование x → Decimal (§9). Money-path (surplus) НЕ здесь (T-F05A-304).
// ============================================================================

#include "domain/vector_qp_solver.hpp"

#include <algorithm>  // std::min
#include <cmath>      // std::isfinite, std::llround, std::pow

namespace cex::matching::domain {

namespace {
/// Нижняя граница q_i при построении D=diag(dHL/q): избегаем деления на ноль.
/// Если q_i ≈ 0, сегмент всё равно запинен box'ом (u_i=0 ⇒ x_i=0), а D_ii берётся
/// от eps — численное упрочнение (ADR-048 §6: q_i→0 не даёт молчаливый мусор).
constexpr double kQFloor = 1e-12;
}  // namespace

QpProblem VectorQpSolver::AssembleProblem(const std::vector<VectorSegment>& segments,
                                          int num_assets) {
  const int I = static_cast<int>(segments.size());
  const int N = std::max(0, num_assets);

  QpProblem prob;
  prob.num_assets = N;
  prob.num_segments = I;

  // W: N×I, столбец j = segments[j].w (в пространстве активов).
  Eigen::MatrixXd W = Eigen::MatrixXd::Zero(N, I);
  Eigen::VectorXd pH = Eigen::VectorXd::Zero(I);   // pH_i = dHL_i
  Eigen::VectorXd qbox = Eigen::VectorXd::Zero(I); // верхняя граница x_i
  Eigen::VectorXd Ddiag = Eigen::VectorXd::Zero(I);// D_ii = dHL_i / q_i

  for (int j = 0; j < I; ++j) {
    const VectorSegment& seg = segments[static_cast<std::size_t>(j)];
    const int rows = std::min<int>(N, static_cast<int>(seg.w.size()));
    for (int i = 0; i < rows; ++i) {
      W(i, j) = seg.w[static_cast<std::size_t>(i)];
    }
    pH(j) = seg.d_hl;                              // ADR-048: pH = dHL
    qbox(j) = std::max(0.0, seg.q_max);
    Ddiag(j) = seg.d_hl / std::max(seg.q_max, kQFloor);  // D = diag(dHL/q)
  }

  // Стандартная форма OSQP (ADR-048):
  //   P = D = diag(dHL/q);  q = −pH;  A = [W; I];  l = [0; 0];  u = [0; q_box].
  prob.P = Eigen::MatrixXd(Ddiag.asDiagonal());
  prob.q = -pH;

  prob.A = Eigen::MatrixXd::Zero(N + I, I);
  if (N > 0) prob.A.topRows(N) = W;
  prob.A.bottomRows(I) = Eigen::MatrixXd::Identity(I, I);

  prob.l = Eigen::VectorXd::Zero(N + I);  // [0_N ; 0_I]
  prob.u = Eigen::VectorXd::Zero(N + I);  // [0_N ; q_box]
  prob.u.tail(I) = qbox;                  // Wx=0 задаётся 0 ≤ Wx ≤ 0

  return prob;
}

cex::common::Decimal VectorQpSolver::Quantize(double value) const {
  if (!std::isfinite(value)) {
    return cex::common::Decimal{0, decimal_scale_};
  }
  const double factor = std::pow(10.0, static_cast<double>(decimal_scale_));
  const long long units = std::llround(value * factor);
  return cex::common::Decimal{static_cast<std::int64_t>(units), decimal_scale_};
}

VectorClearingResult VectorQpSolver::Solve(const std::vector<VectorSegment>& segments,
                                           int num_assets) {
  VectorClearingResult result;

  // Пустая задача: клирить нечего — тривиально сошлось.
  if (segments.empty()) {
    result.status = VectorSolveStatus::kConverged;
    result.residual_norm = 0.0;
    return result;
  }

  const QpProblem prob = AssembleProblem(segments, num_assets);
  const QpSolution sol = backend_.Solve(prob, params_);

  // Backend не дал полезного решения → failed (без частичных денежных проводок).
  if (sol.status == QpBackendStatus::kPrimalInfeasible ||
      sol.status == QpBackendStatus::kDualInfeasible ||
      sol.status == QpBackendStatus::kError) {
    result.status = VectorSolveStatus::kFailed;
    result.iterations = sol.iterations;
    return result;
  }

  const int I = prob.num_segments;
  const int N = prob.num_assets;

  // Защита: backend вернул x неверной длины → failed.
  if (static_cast<int>(sol.x.size()) != I) {
    result.status = VectorSolveStatus::kFailed;
    result.iterations = sol.iterations;
    return result;
  }

  // residual r = W·x (верхние N строк A — это W).
  Eigen::VectorXd r = Eigen::VectorXd::Zero(N);
  if (N > 0) r = prob.A.topRows(N) * sol.x;
  result.residual.assign(r.data(), r.data() + N);
  result.residual_norm = (N > 0) ? r.norm() : 0.0;

  // Квантование x → Decimal (граница денег §9). Крошечные отрицательные (−1e-15
  // от солвера) округляются в 0.
  result.x.reserve(static_cast<std::size_t>(I));
  for (int j = 0; j < I; ++j) {
    result.x.push_back(Quantize(sol.x(j)));
  }
  result.iterations = sol.iterations;

  // Статус: сошёлся при малом остатке; иначе degraded (остаток → surplus,
  // обработка по ADR-047/T-F05A-304, здесь не применяется).
  const bool small_residual = result.residual_norm <= residual_tolerance_;
  if (sol.status == QpBackendStatus::kSolved && small_residual) {
    result.status = VectorSolveStatus::kConverged;
  } else {
    result.status = VectorSolveStatus::kDegraded;
  }

  return result;
}

}  // namespace cex::matching::domain
