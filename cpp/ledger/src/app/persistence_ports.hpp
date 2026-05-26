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
