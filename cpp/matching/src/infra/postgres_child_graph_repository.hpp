#pragma once
// ============================================================================
// postgres_child_graph_repository.hpp — F-09 (MVP-4, ADR-035). Matching infra.
//
// Data-слой OCO/bracket runtime: загрузка ComboGroupState (ноги со статусом +
// граф conditional_links) и идемпотентный персист leg-переходов после grouped
// batch. group_state_transitions.group_id = execution_group_id триггерящего
// batch (ADR-035 §1). Доменные переходы — child_graph_transitions.{hpp,cpp}.
// ============================================================================

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "domain/child_graph_transitions.hpp"

namespace cex::matching::infra {

struct IChildGraphRepository {
  virtual ~IChildGraphRepository() = default;
  /// Загружает граф combo (ноги + рёбра). Пустой parent → пустое состояние.
  virtual domain::ComboGroupState LoadComboGroupState(const std::string& parent_order_id) = 0;
  /// Атомарно применяет переходы к БД: UPDATE статуса/q_max ног + INSERT
  /// group_state_transitions (group_id = execution_group_id, идемпотентно).
  virtual void PersistChildGraphTransitions(
      const std::string& execution_group_id, const std::string& batch_id,
      const domain::ComboGroupState& group,
      const std::vector<domain::GraphTransition>& transitions) = 0;
};

class PostgresChildGraphRepository final : public IChildGraphRepository {
 public:
  using ConnectionFactory = std::function<std::unique_ptr<pqxx::connection>()>;

  explicit PostgresChildGraphRepository(std::string dsn);
  explicit PostgresChildGraphRepository(ConnectionFactory factory);

  domain::ComboGroupState LoadComboGroupState(const std::string& parent_order_id) override;
  void PersistChildGraphTransitions(
      const std::string& execution_group_id, const std::string& batch_id,
      const domain::ComboGroupState& group,
      const std::vector<domain::GraphTransition>& transitions) override;

 private:
  ConnectionFactory connection_factory_;
};

}  // namespace cex::matching::infra
