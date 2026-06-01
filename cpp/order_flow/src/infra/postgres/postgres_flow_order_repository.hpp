#pragma once

#include <functional>
#include <memory>
#include <string>

#include <pqxx/pqxx>

#include "infra/postgres/flow_order_repository_port.hpp"

namespace cex::order_flow::infra {

// PostgreSQL implementation of IFlowOrderRepository.
// Writes flow_orders (status='active') and flow_order_legs in one tx
// so the matching service's PostgresFlowOrderRepository (cpp/matching/
// src/infra/postgres/postgres_flow_order_repository.cpp) can pick the
// order up on the next batch cycle.
class PostgresFlowOrderRepository final : public IFlowOrderRepository {
 public:
  using ConnectionFactory = std::function<std::unique_ptr<pqxx::connection>()>;

  explicit PostgresFlowOrderRepository(std::string connection_string);
  explicit PostgresFlowOrderRepository(ConnectionFactory connection_factory);

  void InsertFlowOrder(const fob::orders::v1::FlowOrder& order) override;

 private:
  ConnectionFactory connection_factory_;
};

}  // namespace cex::order_flow::infra
