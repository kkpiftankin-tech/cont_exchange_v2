#pragma once
// ============================================================================
// postgres_combo_compensation_repository.hpp — F-09 MVP-5 (ADR-037). Matching infra.
//
// Фиксирует требование компенсации при сбое внешней ноги combo
// (external_compensating): внутренние ноги исполнены, внешняя провалилась →
// combo_compensations(pending). Идемпотентно по (parent, leg, report). Сам
// компенсирующий трейд — operator/policy-driven (MVP-6).
// ============================================================================

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <pqxx/pqxx>

#include "cex/common/decimal.hpp"

namespace cex::matching::infra {

struct ComboCompensation {
  std::string parent_order_id;
  std::string leg_id;
  std::string report_id;  ///< внешняя ExecutionReport (idempotency)
  std::string reason;     ///< rejected | timeout | cancelled
  cex::common::Decimal internal_filled_qty{};
};

class PostgresComboCompensationRepository {
 public:
  using ConnectionFactory = std::function<std::unique_ptr<pqxx::connection>()>;

  explicit PostgresComboCompensationRepository(std::string dsn);
  explicit PostgresComboCompensationRepository(ConnectionFactory factory);

  /// Идемпотентно по (parent_order_id, leg_id, report_id). true если вставлено.
  bool RecordPending(const ComboCompensation& c);
  /// Число pending-компенсаций для combo (для проверок/мониторинга).
  int CountPending(const std::string& parent_order_id);
  /// parent_order_id, если leg_id — нога combo (иначе nullopt). Различает
  /// combo-ноги от hedge/прочих internal_order_id в ExecutionReport.
  std::optional<std::string> FindComboLegParent(const std::string& leg_id);

 private:
  ConnectionFactory connection_factory_;
};

}  // namespace cex::matching::infra
