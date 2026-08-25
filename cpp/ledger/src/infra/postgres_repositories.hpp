#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "app/persistence_ports.hpp"

// P2: все PG-репозитории заимствуют соединения из общего пула вместо открытия
// нового pqxx::connection на каждый вызов. Пул объявлен опаковым (LedgerPgPool)
// — pqxx НЕ протекает в этот заголовок. LedgerPgPool — это incomplete type,
// который в .cpp (под guard'ом CEX_LEDGER_HAS_LIBPQXX) определяется как
// наследник cex::common::PgConnectionPool. main.cpp создаёт его через
// MakeLedgerPgPool().
namespace cex::ledger::infra {

// Опаковый тип пула (incomplete type здесь — хранится только через shared_ptr).
class LedgerPgPool;

// Фабрика пула — определена в .cpp. dsn пуст / нет libpqxx → nullptr.
std::shared_ptr<LedgerPgPool> MakeLedgerPgPool(const std::string& dsn,
                                               std::size_t pool_size);

class PostgresPositionsRepository final : public app::PositionsRepositoryPort {
 public:
  explicit PostgresPositionsRepository(
      std::shared_ptr<LedgerPgPool> pool);

  void ApplyFill(const fob::matching::v1::FlowFill& fill) override;

 private:
  std::shared_ptr<LedgerPgPool> pool_;
};

class PostgresLedgerEntriesRepository final : public app::LedgerEntriesRepositoryPort {
 public:
  explicit PostgresLedgerEntriesRepository(
      std::shared_ptr<LedgerPgPool> pool);

  void CreateEntriesForFill(const std::string& batch_id,
                            const fob::matching::v1::FlowFill& fill) override;

 private:
  std::shared_ptr<LedgerPgPool> pool_;
};

class PostgresIdempotencyRepository final : public app::IdempotencyRepositoryPort {
 public:
  explicit PostgresIdempotencyRepository(
      std::shared_ptr<LedgerPgPool> pool);

  bool IsBatchProcessed(const std::string& batch_id) override;
  void MarkBatchProcessed(const std::string& batch_id) override;

 private:
  std::shared_ptr<LedgerPgPool> pool_;
};

class PostgresHedgeLedgerEntriesRepository final : public app::HedgeLedgerEntriesRepositoryPort {
 public:
  explicit PostgresHedgeLedgerEntriesRepository(
      std::shared_ptr<LedgerPgPool> pool);

  void CreateHedgeEntry(const fob::ledger::v1::HedgeExecution& hedge) override;

 private:
  std::shared_ptr<LedgerPgPool> pool_;
};

// F-12 / IN-009 DoD-6 (PR-F12-3c) — accumulator for hedge_pnl + tot_fee
// into PostgreSQL `hedgeflows` row. Used by ledger ApplyExecutionReport.
// No-op gracefully when libpqxx is unavailable.
class PostgresHedgeflowPnlSink final : public app::HedgeflowPnlSinkPort {
 public:
  explicit PostgresHedgeflowPnlSink(
      std::shared_ptr<LedgerPgPool> pool);

  void UpdateHedgePnlDelta(const std::string& hedge_flow_id,
                           const std::string& pnl_delta,
                           const std::string& fee_delta) override;

 private:
  std::shared_ptr<LedgerPgPool> pool_;
};

// ---------------------------------------------------------------------------
// F-06 (T-F06-020) — repositories for the F-06 `positions` / `accounts` tables
// (init.sql §F-06). DISTINCT from the legacy `ledger_positions` repo above.
// Reads support SEQ-LEDGER-001 (GetPositions); ApplyFillTx supports
// SEQ-LEDGER-002 (one TX per fill). All no-op / empty without libpqxx.
// ---------------------------------------------------------------------------
class PostgresPositionRepository final : public app::PositionRepositoryPort {
 public:
  explicit PostgresPositionRepository(
      std::shared_ptr<LedgerPgPool> pool);
  std::vector<app::PositionRow> ListByUser(const std::string& user_id) override;

 private:
  std::shared_ptr<LedgerPgPool> pool_;
};

class PostgresAccountRepository final : public app::AccountRepositoryPort {
 public:
  explicit PostgresAccountRepository(
      std::shared_ptr<LedgerPgPool> pool);
  std::vector<app::AccountRow> ListByUser(const std::string& user_id) override;

 private:
  std::shared_ptr<LedgerPgPool> pool_;
};

// F-06 P0-A (T-F06-070, ADR-044) — mirror legacy reserve/release into the
// F-06 `accounts` table as atomic free<->reserved PG transactions. Built on
// the same shared LedgerPgPool. No-op (returns true) without libpqxx.
class PostgresAccountReserveTx final : public app::AccountReserveTxPort {
 public:
  explicit PostgresAccountReserveTx(
      std::shared_ptr<LedgerPgPool> pool);
  bool ReserveTx(const std::string& user_id,
                 const std::string& asset,
                 const cex::common::Decimal& amount,
                 const std::string& reservation_id) override;
  bool ReleaseTx(const std::string& user_id,
                 const std::string& asset,
                 const cex::common::Decimal& amount,
                 const std::string& reservation_id) override;

 private:
  std::shared_ptr<LedgerPgPool> pool_;
};

class PostgresPositionAccountTx final : public app::PositionAccountTxPort {
 public:
  explicit PostgresPositionAccountTx(
      std::shared_ptr<LedgerPgPool> pool);
  void ApplyFillTx(const app::PositionRow& position,
                   const std::vector<app::AccountRow>& account_deltas) override;

 private:
  std::shared_ptr<LedgerPgPool> pool_;
};

}  // namespace cex::ledger::infra
