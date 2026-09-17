#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cex/common/decimal.hpp"
#include "fob/matching/v1/batch.pb.h"
#include "fob/ledger/v1/ledger.pb.h"

namespace cex::ledger::app {

// ---------------------------------------------------------------------------
// F-06 / F6-5 (T-F06-072, ADR-046) — port для publish'а сигнала
// positions.update «позиции пользователя изменились». Реализация в
// infra/positions_update_publisher (Kafka producer); в тестах — fake.
// Держим интерфейс в app-слое, чтобы app не зависел от Kafka/infra.
// ---------------------------------------------------------------------------
class PositionsUpdatePublisherPort {
 public:
  virtual ~PositionsUpdatePublisherPort() = default;
  // Publish — один сигнал на один user_id. ts_unix — unix-секунды эмиссии.
  // Best-effort: возвращаемое значение информативно, ошибка не валит батч.
  virtual bool Publish(const std::string& user_id,
                       const std::string& batch_id,
                       int64_t ts_unix) = 0;
};

// ---------------------------------------------------------------------------
// F-06 (T-F06-020) — read/write models for the F-06 `positions` / `accounts`
// tables (DISTINCT from the legacy per-currency `ledger_positions`).
// All monetary fields are Decimal (CLAUDE.md §9). side: "long"|"short"|"flat".
// ---------------------------------------------------------------------------
struct AccountRow {
  std::string user_id;
  std::string asset;
  cex::common::Decimal free_balance{0, 0};
  cex::common::Decimal reserved_balance{0, 0};
};

struct PositionRow {
  std::string user_id;
  std::string symbol;
  std::string side{"flat"};
  cex::common::Decimal quantity{0, 0};         // absolute
  cex::common::Decimal avg_entry_price{0, 0};
  cex::common::Decimal unrealized_pnl{0, 0};
  cex::common::Decimal realized_pnl{0, 0};
};

// Port for the F-06 `positions` table (UNIQUE (user_id, symbol)).
class PositionRepositoryPort {
 public:
  virtual ~PositionRepositoryPort() = default;
  // ListByUser — SELECT * FROM positions WHERE user_id = $1 (SEQ-LEDGER-001).
  virtual std::vector<PositionRow> ListByUser(const std::string& user_id) = 0;
};

// Port for the F-06 `accounts` table (UNIQUE (user_id, asset)).
class AccountRepositoryPort {
 public:
  virtual ~AccountRepositoryPort() = default;
  virtual std::vector<AccountRow> ListByUser(const std::string& user_id) = 0;
};

// F-06 (T-F06-022): atomic write of position + account deltas for one fill in
// ONE PostgreSQL transaction (SEQ-LEDGER-002: BEGIN … UPSERT positions … UPDATE
// accounts … COMMIT). Implementations MUST upsert both or neither.
class PositionAccountTxPort {
 public:
  virtual ~PositionAccountTxPort() = default;
  // Upsert the post-fill position snapshot and apply the account balance
  // deltas (free/reserved per asset) atomically. `account_deltas` are signed
  // additive deltas (positive = credit). Empty vector = no balance change.
  virtual void ApplyFillTx(const PositionRow& position,
                           const std::vector<AccountRow>& account_deltas) = 0;
};

// ---------------------------------------------------------------------------
// F-06 P0-A (T-F06-070, ADR-044) — «mirror» of the legacy in-memory reserve
// path into the F-06 `accounts` table. ReserveFunds/ReleaseFunds on the
// LedgerUseCases hot path call these so `accounts.reserved_balance` reflects
// open reservations BEFORE a fill, removing the need to mask underflow with
// GREATEST(0,…) in ApplyFillTx.
//
// Each method is ONE atomic PostgreSQL transaction over a single
// (user_id, asset) row:
//   ReserveTx: free_balance -= amount, reserved_balance += amount.
//   ReleaseTx: reserved_balance -= amount, free_balance += amount.
// `amount` is the (positive) magnitude to move. `reservation_id` is passed for
// logging / future idempotency (a dedicated `reservations` dedup table is a
// separate ticket T-F06-04x per ADR-044 — NOT created here).
// ---------------------------------------------------------------------------
class AccountReserveTxPort {
 public:
  virtual ~AccountReserveTxPort() = default;
  // free -> reserved. Returns false (and logs) if the move would overdraw
  // free_balance (natural accounts_free_balance_nonneg CHECK).
  virtual bool ReserveTx(const std::string& user_id,
                         const std::string& asset,
                         const cex::common::Decimal& amount,
                         const std::string& reservation_id) = 0;
  // reserved -> free. Returns false (and logs) on failure (e.g. would push
  // reserved_balance below zero — accounts_reserved_balance_nonneg CHECK).
  virtual bool ReleaseTx(const std::string& user_id,
                         const std::string& asset,
                         const cex::common::Decimal& amount,
                         const std::string& reservation_id) = 0;
};

// Port for persisting net position effects of each fill.
class PositionsRepositoryPort {
 public:
  virtual ~PositionsRepositoryPort() = default;
  virtual void ApplyFill(const fob::matching::v1::FlowFill& fill) = 0;
};

// Port for persisting accounting/audit entries per fill.
class LedgerEntriesRepositoryPort {
 public:
  virtual ~LedgerEntriesRepositoryPort() = default;
  virtual void CreateEntriesForFill(const std::string& batch_id,
                                    const fob::matching::v1::FlowFill& fill) = 0;
};

// Port for idempotent batch processing checks.
class IdempotencyRepositoryPort {
 public:
  virtual ~IdempotencyRepositoryPort() = default;
  virtual bool IsBatchProcessed(const std::string& batch_id) = 0;
  virtual void MarkBatchProcessed(const std::string& batch_id) = 0;
};

// Port for persisting hedge ledger entries.
class HedgeLedgerEntriesRepositoryPort {
 public:
  virtual ~HedgeLedgerEntriesRepositoryPort() = default;
  virtual void CreateHedgeEntry(const fob::ledger::v1::HedgeExecution& hedge) = 0;
};

// F-12 / IN-009 DoD-6 (PR-F12-3c) — write computed hedge_pnl + fee delta
// back into PostgreSQL `hedgeflows` row for the UI HedgeFlow Monitor.
// pnl_delta and fee_delta are PER-REPORT deltas; the repo MUST do
// COALESCE(... , 0) + delta to accumulate across multiple reports.
// All values are passed as decimal strings to avoid double precision loss.
class HedgeflowPnlSinkPort {
 public:
  virtual ~HedgeflowPnlSinkPort() = default;
  virtual void UpdateHedgePnlDelta(const std::string& hedge_flow_id,
                                   const std::string& pnl_delta,
                                   const std::string& fee_delta) = 0;
};

// ---------------------------------------------------------------------------
// F-18 v2 (T-F18-202/204, ADR-061 §7, ADR-063) — позиция виртуального
// контрагента CE v2 (AGENT: переводчик/арбитражёр). ОТДЕЛЬНАЯ от `accounts`
// (docs/07-data/ce-agent-position.md, party_type-раздел): знаковая, БЕЗ
// CHECK >= 0, ключ (agent_id, asset, venue) — трёхсоставной (прецедент
// sim_positions, ADR-016). `position` = c_j (накопленная клирингом,
// c ← c + f). `in_flight` зарезервировано для Э3 (полоса ±q, T-F18-304) —
// в этой таске всегда 0 (эмиссии ещё нет).
// ---------------------------------------------------------------------------
struct AgentPositionRow {
  std::string agent_id;
  std::string agent_kind;   // "translator" | "arbitrageur" | "" (неизвестно)
  std::string asset;
  std::string venue;
  cex::common::Decimal position{0, 0};    // c_j, ЗНАКОВАЯ, накопленная клирингом
  cex::common::Decimal in_flight{0, 0};   // committed (Э3+), ЗНАКОВАЯ
  std::string last_batch_id;              // последний такт, менявший позицию
  int64_t updated_at_ms{0};
};

// Port for the `ce_agent_position` table (T-F18-204). Единственная мутация —
// накопление c ← c + delta (delta = f_j, сырой поток такта), идемпотентное по
// batch_id (ADR-061 §7: UPSERT-guard `WHERE last_batch_id IS DISTINCT FROM
// $batch_id`). GetPositions — read-only проекция для GetAgentPositions RPC /
// стартовой загрузки write-through кэша LedgerUseCases.
class AgentPositionRepositoryPort {
 public:
  virtual ~AgentPositionRepositoryPort() = default;
  // Применить f_j к (agent_id, asset, venue), защищено guard'ом по batch_id.
  // Возвращает накопленную запись ПОСЛЕ применения (или текущую, если guard
  // отсёк повторную доставку того же batch_id — at-least-once Kafka).
  virtual AgentPositionRow ApplyDelta(const std::string& agent_id,
                                      const std::string& agent_kind,
                                      const std::string& asset,
                                      const std::string& venue,
                                      const cex::common::Decimal& delta,
                                      const std::string& batch_id,
                                      int64_t updated_at_ms) = 0;
  // Прочитать текущие позиции. Пустые векторы фильтров = без фильтра
  // (вернуть все агенты / активы / площадки соответственно).
  virtual std::vector<AgentPositionRow> GetPositions(
      const std::vector<std::string>& agent_ids,
      const std::vector<std::string>& assets,
      const std::vector<std::string>& venues) = 0;
  // F-18 v2 · Э3/Э4 (ADR-061 §4): аддитивно применить сдвиги, вызванные
  // хеджем — НЕ идемпотентно по batch_id (это execution-путь, не клиринг):
  // position += position_delta, in_flight += in_flight_delta. Оба знаковые,
  // в единицах позиции (k-USDT). Строка обязана существовать (создаётся
  // клирингом раньше эмиссии); при отсутствии — no-op. Пустой default для
  // no-op/тестовых репозиториев.
  virtual void ApplyHedge(const std::string& /*agent_id*/,
                          const std::string& /*asset*/,
                          const std::string& /*venue*/,
                          const cex::common::Decimal& /*position_delta*/,
                          const cex::common::Decimal& /*in_flight_delta*/,
                          int64_t /*updated_at_ms*/) {}
};

}  // namespace cex::ledger::app
