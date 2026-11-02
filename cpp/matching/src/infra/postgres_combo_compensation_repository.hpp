#pragma once
// ============================================================================
// postgres_combo_compensation_repository.hpp — F-09 MVP-5 (ADR-037). Matching infra.
//
// Фиксирует требование компенсации при сбое внешней ноги combo
// (external_compensating): внутренние ноги исполнены, внешняя провалилась →
// combo_compensations(pending). Идемпотентно по (parent, leg, report). Сам
// компенсирующий трейд — operator/policy-driven (MVP-6).
// ============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "cex/common/decimal.hpp"

namespace cex::matching::infra {

/// true, если строка — каноничный UUID (8-4-4-4-12, hex). Ноги combo — uuid; идентификаторы CE-хеджей
/// ("ce|band|T_BTC_kraken|BTC|kraken|…") — нет, и передавать их в `$1::uuid` нельзя: PostgreSQL бросает
/// исключение «invalid input syntax for type uuid» (на каждый неудачный хедж — запись ERROR в логе).
inline bool IsUuidText(const std::string& s) {
  if (s.size() != 36) return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

struct ComboCompensation {
  std::string parent_order_id;
  std::string leg_id;
  std::string report_id;  ///< внешняя ExecutionReport (idempotency)
  std::string reason;     ///< rejected | timeout | cancelled
  cex::common::Decimal internal_filled_qty{};
};

/// Pending-компенсация для operator-console (MVP-6).
struct PendingCompensation {
  std::string compensation_id;
  std::string parent_order_id;
  std::string leg_id;
  std::string reason;
  cex::common::Decimal internal_filled_qty{};
  std::int64_t created_at_ms{0};  ///< F-09 MVP-7: epoch-ms для age-gate (ADR-041).
};

class PostgresComboCompensationRepository {
 public:
  using ConnectionFactory = std::function<std::unique_ptr<pqxx::connection>()>;

  explicit PostgresComboCompensationRepository(std::string dsn);
  explicit PostgresComboCompensationRepository(ConnectionFactory factory);

  /// Идемпотентно по (parent_order_id, leg_id, report_id). true если вставлено.
  bool RecordPending(const ComboCompensation& c);
  /// Σ filled_cum ВНУТРЕННИХ ('internal') ног combo, КРОМЕ упавшей внешней ноги
  /// — реальная внутренняя экспозиция, подлежащая откату. 0 → откатывать нечего
  /// (компенсация не нужна). Заменяет ошибочный источник report.filled_qty(),
  /// который у reject'а всегда 0 (см. matching_loop on_external_execution_report).
  cex::common::Decimal SumInternalFilledQty(const std::string& parent_order_id,
                                            const std::string& failed_leg_id);
  /// Число pending-компенсаций для combo (для проверок/мониторинга).
  int CountPending(const std::string& parent_order_id);
  /// parent_order_id, если leg_id — нога combo (иначе nullopt). Различает
  /// combo-ноги от hedge/прочих internal_order_id в ExecutionReport.
  std::optional<std::string> FindComboLegParent(const std::string& leg_id);

  // --- MVP-5 fix: терминальный статус external-ноги после venue-отчёта ---
  // Без этого нога остаётся 'active' → matching re-routes её каждый батч →
  // новые intents/компенсации без конца. Loader исключает терминальные статусы.
  /// Внешняя нога провалилась (reject/cancel/expire) → 'failed_external'.
  /// Идемпотентно (только из не-терминального статуса). true если применено.
  bool MarkExternalLegFailed(const std::string& leg_id);
  /// Внешняя нога исполнена → 'filled' + аккумулирует filled_cum. Идемпотентно
  /// (только из 'active' — повтор отчёта не двоит filled_cum). true если применено.
  bool MarkExternalLegFilled(const std::string& leg_id, const cex::common::Decimal& filled_qty);

  // --- MVP-6 (ADR-039): operator-driven resolution ---
  /// Идемпотентно по status='pending'. action=accept → cancelled; иначе resolved.
  /// true если переход применён (был pending).
  bool ResolvePending(const std::string& compensation_id, const std::string& action,
                      const std::string& operator_id, const std::string& resolving_ref);
  /// Все pending-компенсации (для operator-console).
  std::vector<PendingCompensation> ListPending();
  /// Pending-компенсации одной combo (фильтр для gRPC ListPendingCompensations).
  std::vector<PendingCompensation> ListPending(const std::string& parent_order_id);
  /// Pending-компенсация по id (T-F09-065). nullopt если нет pending с таким id.
  std::optional<PendingCompensation> GetPending(const std::string& compensation_id);

 private:
  ConnectionFactory connection_factory_;
};

}  // namespace cex::matching::infra
