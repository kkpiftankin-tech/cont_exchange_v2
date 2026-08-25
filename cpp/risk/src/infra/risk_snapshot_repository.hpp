#pragma once
// ============================================================================
// risk_snapshot_repository.hpp — PostgreSQL DAO для F-06 buildRiskSnapshot
// (T-F06-031 / SEQ-RISK-001).
//
// Ответственность:
//   * чтение позиций пользователя (positions, side != 'flat');
//   * чтение коллатерал-балансов (accounts);
//   * резолв margin rates (risk_limits: user → role → global, с дефолтом);
//   * запись RiskSnapshot строки (risk_snapshots);
//   * чтение последнего снапшота для GetRiskSnapshot (ORDER BY timestamp DESC).
//
// Паттерн подключения — как ledger (pqxx::connection per-op, exec_params,
// CEX_RISK_HAS_LIBPQXX guard). DSN из env RISK_POSTGRES_DSN. Если libpqxx
// недоступен или DSN пуст — методы возвращают пустые результаты / false
// (graceful degradation, как ledger без persistence).
//
// Decimal <-> NUMERIC через infra/postgres/decimal_conversion (§9 — никаких
// double для money).
// ============================================================================

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cex/common/decimal.hpp"

namespace cex::risk::infra {

using cex::common::Decimal;

// ADR-045: opaque-тип пула соединений (определён в .cpp; pqxx не протекает сюда).
class RiskPgPool;

// Строка positions для одного открытого инструмента пользователя.
struct PositionRow {
  std::string symbol;
  std::string side;                          // 'long' | 'short'
  Decimal quantity{Decimal::zero()};
  Decimal avg_entry_price{Decimal::zero()};
  Decimal unrealized_pnl{Decimal::zero()};
};

// Агрегаты по accounts пользователя.
struct CollateralRow {
  Decimal free_balance{Decimal::zero()};      // Σ free_balance
  Decimal reserved_balance{Decimal::zero()};  // Σ reserved_balance
};

// Margin rates, резолвленные из risk_limits (с дефолтами если строки нет).
struct ResolvedRates {
  Decimal initial_margin_rate{Decimal::zero()};
  Decimal maintenance_margin_rate{Decimal::zero()};
};

// Данные для записи строки в risk_snapshots.
struct RiskSnapshotInsert {
  std::string entity_id;
  // T-F06-073 idempotency: batch_id источника снапшота. Пустая строка → batch_id
  // пишется как SQL NULL (снапшот не привязан к батчу, дедуп не применяется).
  std::string batch_id;
  Decimal free_collateral{Decimal::zero()};
  Decimal reserved_collateral{Decimal::zero()};
  Decimal initial_margin{Decimal::zero()};
  Decimal maintenance_margin{Decimal::zero()};
  bool throttled{false};
  bool margin_call{false};
  bool liquidation{false};
};

// Результат InsertSnapshot (T-F06-073). Caller использует kInserted чтобы НЕ
// дублировать margin-call alert при повторной доставке (kDuplicate).
enum class InsertResult {
  kInserted,   // строка реально вставлена (новый (entity_id, batch_id))
  kDuplicate,  // ON CONFLICT DO NOTHING сработал — дубликат, alert не слать
  kError,      // ошибка записи (offset не коммитим, alert не слать)
};

// Прочитанная строка risk_snapshots (для GetRiskSnapshot).
struct RiskSnapshotRow {
  std::string entity_id;
  Decimal free_collateral{Decimal::zero()};
  Decimal reserved_collateral{Decimal::zero()};
  Decimal initial_margin{Decimal::zero()};
  Decimal maintenance_margin{Decimal::zero()};
  bool margin_call{false};
  bool liquidation{false};
  std::int64_t timestamp_unix{0};  // epoch seconds (для proto Timestamp)
};

// PostgreSQL-реализация DAO. enabled() == false если нет DSN / нет libpqxx.
class RiskSnapshotRepository {
 public:
  explicit RiskSnapshotRepository(std::string conn_str);

  bool enabled() const { return enabled_; }

  // Открытые позиции пользователя (side != 'flat'). Пустой вектор если нет.
  std::vector<PositionRow> ListOpenPositions(const std::string& user_id);

  // Сумма free/reserved balance по всем accounts пользователя.
  CollateralRow LoadCollateral(const std::string& user_id);

  // Margin rates: ищет risk_limits для (user, user_id) → (role, role) →
  // (global, 'global'). role резолвится best-effort; при отсутствии любых
  // строк возвращает дефолт (initial=0.1, maintenance=0.05).
  ResolvedRates ResolveRates(const std::string& user_id, const std::string& role);

  // Запись строки снапшота с дедупом по (entity_id, batch_id) — ON CONFLICT
  // DO NOTHING (T-F06-073). Возвращает:
  //   kInserted  — строка вставлена (caller может публиковать alert);
  //   kDuplicate — повтор того же batch (alert НЕ слать повторно);
  //   kError     — ошибка / degraded mode (offset не коммитим).
  InsertResult InsertSnapshot(const RiskSnapshotInsert& snap);

  // Последний снапшот для entity_id (ORDER BY timestamp DESC LIMIT 1).
  std::optional<RiskSnapshotRow> LatestSnapshot(const std::string& entity_id);

 private:
  // ADR-045: единый пул PG-соединений (opaque — pqxx не протекает в заголовок,
  // как в ledger). Создаётся из conn_str в конструкторе; nullptr в degraded mode.
  std::shared_ptr<RiskPgPool> pool_;
  bool enabled_{false};
};

}  // namespace cex::risk::infra
