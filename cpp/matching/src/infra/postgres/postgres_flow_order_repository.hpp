#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "domain/flow_order_repository.hpp"

namespace cex::matching::infra {

class PostgresFlowOrderRepository final : public domain::IFlowOrderRepository {
 public:
  using ConnectionFactory = std::function<std::unique_ptr<pqxx::connection>()>;

  explicit PostgresFlowOrderRepository(std::string connection_string);
  explicit PostgresFlowOrderRepository(ConnectionFactory connection_factory);

  std::vector<domain::FlowOrder> LoadActiveFlowOrders(
      std::chrono::system_clock::time_point as_of_time,
      const std::optional<std::string>& instrument_filter = std::nullopt,
      const std::optional<std::int32_t>& limit = std::nullopt) override;

  void UpdateFilledVolumes(
      const std::vector<domain::OrderFillDelta>& deltas,
      std::chrono::system_clock::time_point batch_time) override;

 private:
  ConnectionFactory connection_factory_;
};

}  // namespace cex::matching::infra
