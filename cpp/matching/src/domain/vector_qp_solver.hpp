#pragma once
// ============================================================================
// vector_qp_solver.hpp — F-05A (T-F05A-302 / T-F05A-303). Matching domain.
//
// QP vector-clearing solver для внешней векторной ликвидности (F-05A).
// Внешние orderbook-уровни нормализуются в flow-сегменты (столбцы матрицы W в
// пространстве активов); клиринг = QP:
//
//     max_x  xᵀ·pH − ½·xᵀ·D·x     s.t.  W·x = 0,  0 ≤ x ≤ q
//
// где D = diag(dHL_i / q_i) (SPD), pH_i = dHL_i. Отображение в стандартную форму
// OSQP (min ½xᵀPx + qᵀx s.t. l ≤ A x ≤ u) — по ADR-048:
//
//     P = D                 q = −pH
//     A = [ W ; I ]         l = [ 0 ; 0 ]        u = [ 0 ; q_box ]
//     (Wx=0 кодируется как 0 ≤ Wx ≤ 0)
//
// Реальный QP решает backend (OSQP) за портом IQpBackend (ADR-048: Eigen только
// собирает/масштабирует W, D; сам QP — OSQP). Здесь — сборка standard-form,
// вызов backend, residual r=Wx, диагностика и КВАНТОВАНИЕ x → Decimal на выходе
// (граница денег §9 / ADR-005). Money-path (surplus-проводки) — отдельно
// (T-F05A-304, заблокирован ADR-047 `proposed`); здесь остаток лишь измеряется.
//
// Детерминизм (ADR-048 / AC-F05A-011): фиксированные QP-параметры, adaptive-rho
// off, без таймеров/random — результат бит-в-бит воспроизводим на тех же входах.
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include <eigen3/Eigen/Dense>

#include "cex/common/decimal.hpp"

namespace cex::matching::domain {

// ----------------------------------------------------------------------------
// Вход: flow-сегмент = один столбец матрицы W (в пространстве активов).
// Числовые поля — double: сборка/масштабирование матриц идёт в double (§9
// допускает double ВНУТРИ солвера); денежная граница — только на выходе (x →
// Decimal). Маппинг из proto VectorFlowSegment (Decimal) в этот struct — в
// адаптере интеграции (T-F05A-305).
// ----------------------------------------------------------------------------
struct VectorSegment {
  std::string segment_id;        ///< id сегмента (для трассировки x_i → сегмент)
  std::string source_order_id;   ///< исходный ордер/уровень
  std::vector<double> w;         ///< столбец W, длина = num_assets (знаковые flows)
  double d_hl{0.0};              ///< dHL_i = p_high_i > 0 (кривизна и pH_i)
  double q_max{0.0};             ///< верхняя граница box для x_i (>= 0)
};

// ----------------------------------------------------------------------------
// QP в стандартной форме OSQP: min ½ xᵀ P x + qᵀ x  s.t.  l ≤ A x ≤ u.
// Плотные Eigen-матрицы (сборка/тесты); backend конвертирует в свой формат.
// ----------------------------------------------------------------------------
struct QpProblem {
  Eigen::MatrixXd P;  ///< I×I, SPD (= D)
  Eigen::VectorXd q;  ///< I     (= −pH)
  Eigen::MatrixXd A;  ///< (N+I)×I  (= [W; I])
  Eigen::VectorXd l;  ///< N+I   (= [0; 0])
  Eigen::VectorXd u;  ///< N+I   (= [0; q_box])
  int num_assets{0};  ///< N (число строк W)
  int num_segments{0};///< I (число столбцов W)
};

// ----------------------------------------------------------------------------
// Порт QP-backend (реализуется OSQP-адаптером в T-F05A-301; в тестах — fake).
// ----------------------------------------------------------------------------
enum class QpBackendStatus {
  kSolved,           ///< сошёлся в пределах eps
  kMaxIterReached,   ///< достигнут max_iter (частичное решение)
  kPrimalInfeasible, ///< ограничения несовместны
  kDualInfeasible,   ///< задача не ограничена
  kError             ///< числовой/иной сбой backend
};

/// Детерминированные параметры QP (ADR-048): фиксированный max_iter, adaptive-rho
/// off, фиксированные tolerances. Никаких таймеров/random.
struct QpParams {
  int max_iter{4000};
  double eps_abs{1e-9};
  double eps_rel{1e-9};
  bool adaptive_rho{false};  ///< ОБЯЗАТЕЛЬНО false для replay-детерминизма
};

struct QpSolution {
  Eigen::VectorXd x;                              ///< решение (длина I)
  std::uint32_t iterations{0};
  QpBackendStatus status{QpBackendStatus::kError};
};

struct IQpBackend {
  virtual ~IQpBackend() = default;
  /// Решить QP в стандартной форме. Детерминирован при фиксированных params.
  virtual QpSolution Solve(const QpProblem& problem, const QpParams& params) = 0;
};

// ----------------------------------------------------------------------------
// Результат векторного клиринга. x квантован в Decimal (граница денег §9).
// residual/residual_norm — диагностика (double допустим, ADR-048/§9).
// ----------------------------------------------------------------------------
enum class VectorSolveStatus {
  kConverged,  ///< ||Wx|| ≤ tol и backend решил
  kDegraded,   ///< backend решил, но ||Wx|| > tol (остаток → surplus, T-F05A-304)
  kFailed      ///< backend не решил (infeasible/unbounded/error)
};

struct VectorClearingResult {
  std::vector<cex::common::Decimal> x;  ///< executed rate на сегмент, квантован
  std::vector<double> residual;         ///< r = W·x по каждому активу (диагностика)
  double residual_norm{0.0};            ///< ||r||₂
  std::uint32_t iterations{0};
  VectorSolveStatus status{VectorSolveStatus::kFailed};
};

struct IVectorClearingSolver {
  virtual ~IVectorClearingSolver() = default;
  virtual VectorClearingResult Solve(const std::vector<VectorSegment>& segments,
                                     int num_assets) = 0;
};

// ----------------------------------------------------------------------------
// Реализация: сборка standard-form (ADR-048) + backend + residual + квантование.
// ----------------------------------------------------------------------------
class VectorQpSolver final : public IVectorClearingSolver {
 public:
  /// backend хранится по ссылке (владелец — caller, как solver_ в MatchingLoop).
  explicit VectorQpSolver(IQpBackend& backend,
                          QpParams params = {},
                          double residual_tolerance = 1e-9,
                          std::int32_t decimal_scale = 12)
      : backend_(backend),
        params_(params),
        residual_tolerance_(residual_tolerance),
        decimal_scale_(decimal_scale) {}

  VectorClearingResult Solve(const std::vector<VectorSegment>& segments,
                             int num_assets) override;

  /// Сборка QP в стандартной форме по ADR-048. Чистая функция — юнит-тестируема
  /// независимо от backend. `num_assets` = N (строки W); столбцы = segments.size().
  static QpProblem AssembleProblem(const std::vector<VectorSegment>& segments,
                                   int num_assets);

 private:
  /// double → Decimal{units, scale} half-to-even-agnostic round (llround),
  /// детерминировано. Граница денег на выходе солвера (§9 / ADR-005).
  cex::common::Decimal Quantize(double value) const;

  IQpBackend& backend_;
  QpParams params_;
  double residual_tolerance_;
  std::int32_t decimal_scale_;
};

}  // namespace cex::matching::domain
