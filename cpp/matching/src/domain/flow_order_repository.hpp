#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/flow_order.hpp"
#include "domain/order_fill_delta.hpp"

namespace cex::matching::domain {

class IFlowOrderRepository {
 public:
  virtual ~IFlowOrderRepository() = default;

  // instrument_filter returns orders that have at least one leg for the instrument symbol.
  virtual std::vector<FlowOrder> LoadActiveFlowOrders(
      std::chrono::system_clock::time_point as_of_time,
      const std::optional<std::string>& instrument_filter = std::nullopt,
      const std::optional<std::int32_t>& limit = std::nullopt) = 0;

  // deltas update order-level cumulative fill in portfolio units.
  virtual void UpdateFilledVolumes(
      const std::vector<OrderFillDelta>& deltas,
      std::chrono::system_clock::time_point batch_time) = 0;
};

}  // namespace cex::matching::domain
