// ============================================================================
// osqp_backend.cpp — F-05A (T-F05A-301). См. заголовок в .hpp.
// Конвертация плотной стандартной формы (Eigen) → CSC → OSQP solve. OSQP 0.6.x.
// ============================================================================

#include "infra/osqp_backend.hpp"

#include <vector>

#include "osqp.h"

namespace cex::matching::infra {

namespace dm = cex::matching::domain;

namespace {

/// Плотная колонка-мажорная матрица Eigen → CSC (nz=-1). Только для A (все
/// ненулевые). Возвращает через out-параметры массивы p/i/x (c_int/c_float).
void ToCsc(const Eigen::MatrixXd& M, std::vector<c_int>& p, std::vector<c_int>& i,
           std::vector<c_float>& x) {
  const int rows = static_cast<int>(M.rows());
  const int cols = static_cast<int>(M.cols());
  p.assign(static_cast<std::size_t>(cols) + 1, 0);
  i.clear();
  x.clear();
  for (int c = 0; c < cols; ++c) {
    for (int r = 0; r < rows; ++r) {
      const double v = M(r, c);
      if (v != 0.0) {
        i.push_back(static_cast<c_int>(r));
        x.push_back(static_cast<c_float>(v));
      }
    }
    p[static_cast<std::size_t>(c) + 1] = static_cast<c_int>(i.size());
  }
}

/// Верхнетреугольная часть P (OSQP требует upper-triangular) → CSC.
void ToCscUpper(const Eigen::MatrixXd& M, std::vector<c_int>& p,
                std::vector<c_int>& i, std::vector<c_float>& x) {
  const int n = static_cast<int>(M.cols());
  p.assign(static_cast<std::size_t>(n) + 1, 0);
  i.clear();
  x.clear();
  for (int c = 0; c < n; ++c) {
    for (int r = 0; r <= c; ++r) {  // i <= j
      const double v = M(r, c);
      if (v != 0.0) {
        i.push_back(static_cast<c_int>(r));
        x.push_back(static_cast<c_float>(v));
      }
    }
    p[static_cast<std::size_t>(c) + 1] = static_cast<c_int>(i.size());
  }
}

dm::QpBackendStatus MapStatus(c_int status_val) {
  switch (status_val) {
    case OSQP_SOLVED:
    case OSQP_SOLVED_INACCURATE:
      return dm::QpBackendStatus::kSolved;
    case OSQP_MAX_ITER_REACHED:
      return dm::QpBackendStatus::kMaxIterReached;
    case OSQP_PRIMAL_INFEASIBLE:
    case OSQP_PRIMAL_INFEASIBLE_INACCURATE:
      return dm::QpBackendStatus::kPrimalInfeasible;
    case OSQP_DUAL_INFEASIBLE:
    case OSQP_DUAL_INFEASIBLE_INACCURATE:
      return dm::QpBackendStatus::kDualInfeasible;
    default:
      return dm::QpBackendStatus::kError;
  }
}

}  // namespace

dm::QpSolution OsqpBackend::Solve(const dm::QpProblem& problem,
                                  const dm::QpParams& params) {
  dm::QpSolution out;

  const int n = static_cast<int>(problem.P.cols());  // число переменных (I)
  const int m = static_cast<int>(problem.A.rows());  // число ограничений (N+I)

  // Валидация размерностей — иначе OSQP получит некорректные данные.
  if (n == 0 || problem.P.rows() != n || static_cast<int>(problem.A.cols()) != n ||
      static_cast<int>(problem.q.size()) != n ||
      static_cast<int>(problem.l.size()) != m ||
      static_cast<int>(problem.u.size()) != m) {
    out.status = dm::QpBackendStatus::kError;
    return out;
  }

  // CSC-представления (массивы должны жить до конца osqp_setup — он копирует).
  std::vector<c_int> Pp, Pi, Ap, Ai;
  std::vector<c_float> Px, Ax;
  ToCscUpper(problem.P, Pp, Pi, Px);
  ToCsc(problem.A, Ap, Ai, Ax);

  std::vector<c_float> qv(problem.q.data(), problem.q.data() + n);
  std::vector<c_float> lv(problem.l.data(), problem.l.data() + m);
  std::vector<c_float> uv(problem.u.data(), problem.u.data() + m);

  csc P_csc{};
  P_csc.nzmax = static_cast<c_int>(Px.size());
  P_csc.m = n;
  P_csc.n = n;
  P_csc.p = Pp.data();
  P_csc.i = Pi.data();
  P_csc.x = Px.data();
  P_csc.nz = -1;  // CSC (compressed-column)

  csc A_csc{};
  A_csc.nzmax = static_cast<c_int>(Ax.size());
  A_csc.m = m;
  A_csc.n = n;
  A_csc.p = Ap.data();
  A_csc.i = Ai.data();
  A_csc.x = Ax.data();
  A_csc.nz = -1;

  OSQPData data{};
  data.n = n;
  data.m = m;
  data.P = &P_csc;
  data.q = qv.data();
  data.A = &A_csc;
  data.l = lv.data();
  data.u = uv.data();

  OSQPSettings settings{};
  osqp_set_default_settings(&settings);
  // Детерминизм (ADR-048): без адаптивного rho, фиксированный max_iter/tol,
  // без warm-start. verbose off. polish — детерминированное уточнение.
  settings.adaptive_rho = 0;
  settings.max_iter = static_cast<c_int>(params.max_iter);
  settings.eps_abs = static_cast<c_float>(params.eps_abs);
  settings.eps_rel = static_cast<c_float>(params.eps_rel);
  settings.warm_start = 0;
  settings.verbose = 0;
  settings.polish = 1;

  OSQPWorkspace* work = nullptr;
  const c_int setup_flag = osqp_setup(&work, &data, &settings);
  if (setup_flag != 0 || work == nullptr) {
    if (work != nullptr) osqp_cleanup(work);
    out.status = dm::QpBackendStatus::kError;
    return out;
  }

  osqp_solve(work);

  out.iterations = static_cast<std::uint32_t>(work->info->iter);
  out.status = MapStatus(work->info->status_val);

  // Копируем решение x (длина n).
  out.x = Eigen::VectorXd::Zero(n);
  if (work->solution != nullptr && work->solution->x != nullptr) {
    for (int j = 0; j < n; ++j) {
      out.x(j) = static_cast<double>(work->solution->x[j]);
    }
  }

  osqp_cleanup(work);
  return out;
}

}  // namespace cex::matching::infra
