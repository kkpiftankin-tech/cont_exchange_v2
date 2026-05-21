#pragma once

#include <string>

#include "fob/matching/v1/batch.pb.h"
#include "fob/ledger/v1/ledger.pb.h"

namespace cex::ledger::app {

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

}  // namespace cex::ledger::app
