#pragma once
#include <vector>

#include "fob/orders/v1/orders.pb.h"
#include "fob/matching/v1/batch.pb.h"

namespace cex::matching::domain {

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
      const std::vector<fob::orders::v1::FlowOrder>& active_orders) = 0;
};

}  // namespace cex::matching::domain
