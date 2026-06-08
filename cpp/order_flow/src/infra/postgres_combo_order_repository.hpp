#pragma once
// ============================================================================
// postgres_combo_order_repository.hpp — F-09 (T-F09-034).
//
// Репозиторий order_flow для 5 OLTP-таблиц F-09: batch_orders, combo_orders,
// combo_order_legs, combo_constraints, conditional_links. INSERT parent + ноги
// + ограничения + граф в ОДНОЙ транзакции (атомарно, AC-F09-001). Domain-типы
// (combo_order.hpp) — на входе; proto на этом слое не используется (ADR-032).
//
// Идемпотентность: ON CONFLICT (PK) DO NOTHING. Ключ идемпотентности —
// combo_order_id; маппинг client_combo_id → combo_order_id выполняет
// CreateComboOrderUseCase (T-F09-031). См. NOTE в .cpp.
// ============================================================================

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "domain/combo_order.hpp"

namespace cex::order_flow::infra {

/// Порт записи combo-заявок (DIP — app зависит от интерфейса, не от PG).
class IComboOrderRepository {
 public:
  virtual ~IComboOrderRepository() = default;

  /// Атомарно сохраняет combo (+ опциональный родительский batch) со всеми
  /// ногами, ограничениями и conditional-ссылками. Идемпотентно по PK.
  virtual void InsertComboOrder(const domain::ComboOrder& combo,
                                const std::optional<domain::BatchOrder>& batch) = 0;

  /// Обновляет статус combo-заявки (например draft → active / rejected).
  virtual void UpdateComboStatus(const std::string& combo_order_id,
                                 domain::ParentOrderStatus status) = 0;

  /// Текущий статус combo (nullopt — заявка не найдена). Для idempotent cancel.
  virtual std::optional<domain::ParentOrderStatus> GetComboStatus(
      const std::string& combo_order_id) = 0;

  /// leg_id активных (не cancelled/filled) ног — для отмены дочерних FlowOrder.
  virtual std::vector<std::string> GetActiveLegIds(const std::string& combo_order_id) = 0;

  /// Атомарно переводит combo и его активные ноги в cancelled (одна транзакция).
  virtual void CancelComboAndLegs(const std::string& combo_order_id) = 0;
};

/// PostgreSQL-реализация (libpqxx), зеркалит стиль PostgresFlowOrderRepository.
class PostgresComboOrderRepository final : public IComboOrderRepository {
 public:
  using ConnectionFactory = std::function<std::unique_ptr<pqxx::connection>()>;

  explicit PostgresComboOrderRepository(std::string connection_string);
  explicit PostgresComboOrderRepository(ConnectionFactory connection_factory);

  void InsertComboOrder(const domain::ComboOrder& combo,
                        const std::optional<domain::BatchOrder>& batch) override;
  void UpdateComboStatus(const std::string& combo_order_id,
                         domain::ParentOrderStatus status) override;
  std::optional<domain::ParentOrderStatus> GetComboStatus(
      const std::string& combo_order_id) override;
  std::vector<std::string> GetActiveLegIds(const std::string& combo_order_id) override;
  void CancelComboAndLegs(const std::string& combo_order_id) override;

 private:
  ConnectionFactory connection_factory_;
};

}  // namespace cex::order_flow::infra
