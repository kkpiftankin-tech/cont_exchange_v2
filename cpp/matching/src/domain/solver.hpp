#pragma once
#include <unordered_map>
#include <vector>

#include "fob/common/v1/common.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "domain/flow_order.hpp"
#include "fob/orders/v1/orders.pb.h"
#include "fob/venue/v1/venue.pb.h"

namespace cex::matching::domain {

using ExternalLiquidityBySymbol =
    std::unordered_map<std::string, fob::venue::v1::VenueLiquidityCurve>;

// Placeholder interface for the real continuous clearing optimizer.
// This is exactly the place where you will implement the LP/QP formulation:
// - decision variables: executed speeds x_i (or dq_i for dt)
// - constraints: 0 <= x_i <= u_i, inventory/budget constraints, etc.
// - objective: maximize welfare / minimize convex cost / add regularization.
//
// In MVP we use a simulator (see app/matching_loop.cpp).
struct IContinuousClearingSolver {
  virtual ~IContinuousClearingSolver() = default;

  // Solve one clearing step for the active flow orders.
  // Returns BatchResult.fills + executed_rates + clear_prices etc.
  virtual fob::matching::v1::BatchResult Solve(
      const std::vector<domain::FlowOrder>& active_orders,
      const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices,
      const ExternalLiquidityBySymbol& external_liquidity = {}) = 0;
};

}  // namespace cex::matching::domain
