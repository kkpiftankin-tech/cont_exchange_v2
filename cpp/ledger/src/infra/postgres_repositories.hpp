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

}  // namespace cex::ledger::infra