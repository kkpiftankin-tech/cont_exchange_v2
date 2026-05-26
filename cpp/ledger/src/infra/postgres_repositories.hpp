#pragma once

#include <string>

#include "app/persistence_ports.hpp"

namespace cex::ledger::infra {

class PostgresPositionsRepository final : public app::PositionsRepositoryPort {
 public:
  explicit PostgresPositionsRepository(std::string conn_str);

  void ApplyFill(const fob::matching::v1::FlowFill& fill) override;

 private:
  std::string conn_str_;
};

class PostgresLedgerEntriesRepository final : public app::LedgerEntriesRepositoryPort {
 public:
  explicit PostgresLedgerEntriesRepository(std::string conn_str);

  void CreateEntriesForFill(const std::string& batch_id,
                            const fob::matching::v1::FlowFill& fill) override;

 private:
  std::string conn_str_;
};

class PostgresIdempotencyRepository final : public app::IdempotencyRepositoryPort {
 public:
  explicit PostgresIdempotencyRepository(std::string conn_str);

  bool IsBatchProcessed(const std::string& batch_id) override;
  void MarkBatchProcessed(const std::string& batch_id) override;

 private:
  std::string conn_str_;
};

class PostgresHedgeLedgerEntriesRepository final : public app::HedgeLedgerEntriesRepositoryPort {
 public:
  explicit PostgresHedgeLedgerEntriesRepository(std::string conn_str);

  void CreateHedgeEntry(const fob::ledger::v1::HedgeExecution& hedge) override;

 private:
  std::string conn_str_;
};

// F-12 / IN-009 DoD-6 (PR-F12-3c) — accumulator for hedge_pnl + tot_fee
// into PostgreSQL `hedgeflows` row. Used by ledger ApplyExecutionReport.
// No-op gracefully when libpqxx is unavailable.
class PostgresHedgeflowPnlSink final : public app::HedgeflowPnlSinkPort {
 public:
  explicit PostgresHedgeflowPnlSink(std::string conn_str);

  void UpdateHedgePnlDelta(const std::string& hedge_flow_id,
                           const std::string& pnl_delta,
                           const std::string& fee_delta) override;

 private:
  std::string conn_str_;
};

}  // namespace cex::ledger::infra