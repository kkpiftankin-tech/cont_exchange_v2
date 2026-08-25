#pragma once
// ============================================================================
// postgres_execution_groups_repository.hpp — F-09 (T-F09-047). Matching infra.
//
// Персистит ExecutionGroup в PostgreSQL одной транзакцией:
//   1) INSERT execution_groups        (ON CONFLICT execution_group_id DO NOTHING)
//   2) INSERT group_state_transitions (ON CONFLICT idempotency_key DO NOTHING)
//   3) UPDATE combo_orders.status      (partial/filled/degraded по group_status)
// Идемпотентно по execution_group_id и idempotency_key (at-least-once safe).
// Принимает canonical proto ExecutionGroup (тот же объект, что в Kafka).
// ============================================================================

#include <functional>
#include <memory>
#include <string>

#include <pqxx/pqxx>

#include "fob/matching/v1/execution_group.pb.h"

namespace cex::matching::infra {

struct IExecutionGroupsRepository {
  virtual ~IExecutionGroupsRepository() = default;
  /// Атомарно сохраняет группу исполнения + переход статуса + статус combo.
  virtual void PersistExecutionGroup(const fob::matching::v1::ExecutionGroup& eg) = 0;
};

class PostgresExecutionGroupsRepository final : public IExecutionGroupsRepository {
 public:
  using ConnectionFactory = std::function<std::unique_ptr<pqxx::connection>()>;

  explicit PostgresExecutionGroupsRepository(std::string dsn);
  explicit PostgresExecutionGroupsRepository(ConnectionFactory factory);

  void PersistExecutionGroup(const fob::matching::v1::ExecutionGroup& eg) override;

 private:
  ConnectionFactory connection_factory_;
};

}  // namespace cex::matching::infra
