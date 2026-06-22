#pragma once
// ============================================================================
// active_grouped_orders_loader.hpp — F-09 (T-F09-048). Matching infra.
//
// Загружает активные combo-группы режима multileg_vector_solver из PostgreSQL
// (combo_orders + combo_order_legs) и строит domain MultiLegVectorOrder[] для
// grouped solver. orchestration_only НЕ загружается (идёт независимыми FlowOrder
// через orders.normalized, см. MVP-1). Внешние капы (liq/risk/venue) здесь не
// заполняются — их добавляет batch-цикл из market data / risk на каждом шаге.
// ============================================================================

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "domain/multileg_vector_order.hpp"

namespace cex::matching::infra {

struct IActiveGroupsLoader {
  virtual ~IActiveGroupsLoader() = default;
  virtual std::vector<domain::MultiLegVectorOrder> LoadActiveGroups() = 0;
};

class PostgresActiveGroupsLoader final : public IActiveGroupsLoader {
 public:
  using ConnectionFactory = std::function<std::unique_ptr<pqxx::connection>()>;

  explicit PostgresActiveGroupsLoader(std::string dsn);
  explicit PostgresActiveGroupsLoader(ConnectionFactory factory);

  std::vector<domain::MultiLegVectorOrder> LoadActiveGroups() override;

 private:
  ConnectionFactory connection_factory_;
};

}  // namespace cex::matching::infra
