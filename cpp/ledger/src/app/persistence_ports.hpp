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

}  // namespace cex::ledger::app
