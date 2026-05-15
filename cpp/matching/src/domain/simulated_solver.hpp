#pragma once

#include "domain/solver.hpp"

namespace cex::matching::domain {

class SimulatedContinuousClearingSolver : public IContinuousClearingSolver {
 public:
  explicit SimulatedContinuousClearingSolver(int batch_interval_ms);
  void set_batch_interval_ms(int batch_interval_ms);

  fob::matching::v1::BatchResult Solve(
      const std::vector<fob::orders::v1::FlowOrder>& active_orders,
      const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices,
      const ExternalLiquidityBySymbol& external_liquidity = {}) override;

  void SetBatchIntervalMs(int batch_interval_ms);

 private:
  int batch_interval_ms_;
};

}  // namespace cex::matching::domain
