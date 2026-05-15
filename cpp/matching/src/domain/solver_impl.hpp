#pragma once

#include <tuple>
#include <unordered_map>

#include "domain/solver.hpp"
#include "domain/solver_config.hpp"

#include <eigen3/Eigen/Sparse>
#include <eigen3/Eigen/Cholesky>

namespace cex::matching::domain {

class ContinuousClearingSolver : public IContinuousClearingSolver {
 public:
  ContinuousClearingSolver();

  fob::matching::v1::BatchResult Solve(
      const std::vector<FlowOrder>& active_orders,
      const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices,
      const ExternalLiquidityBySymbol& external_liquidity = {}) override;

  void SetSolverConfig(SolverConfig cfg);

 private:
    Eigen::VectorXd SolveImpl(
        Eigen::SparseMatrix<double> &W,
        Eigen::VectorXd &pi,
        Eigen::VectorXd &d,
        Eigen::VectorXd &qH,
        Eigen::VectorXd &pH
    );

    std::tuple<Eigen::SparseMatrix<double>, Eigen::VectorXd, Eigen::VectorXd, Eigen::VectorXd, Eigen::VectorXd>
    Init(
        const std::vector<FlowOrder> &orders,
        const std::unordered_map<std::string, fob::common::v1::Decimal> &reference_prices,
        const std::unordered_map<std::string, size_t> &prices_map
    );

  SolverConfig cfg_;
};

}  // namespace cex::matching::domain
