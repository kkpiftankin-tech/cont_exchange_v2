#pragma once

#include <cstdint>
#include <string>

#include "fob/execution/v1/execution.pb.h"

namespace cex::venues::infra {

// PostgreSQL persistence for the `hedgeflows` table (F-12 / IN-008 DoD-4).
// One row per ExecutionIntent. Compiled in only when libpqxx is available
// (CEX_VENUES_HAS_LIBPQXX). All methods are best-effort and log on failure
// — they do not throw, so a PG outage degrades F-12 to "matching+venues
// only" without breaking the hot path.
class PostgresHedgeflowRepository final {
 public:
  explicit PostgresHedgeflowRepository(std::string connection_string);

  // CREATE TABLE IF NOT EXISTS — keeps the C++ side self-healing if
  // infra/postgres/init.sql wasn't applied (mirrors the pattern used by
  // PostgresVenueConfigRepository).
  bool EnsureSchema();

  // Insert row with status='OPEN' on intent arrival. ON CONFLICT
  // (hedge_flow_id) DO NOTHING — safe against Kafka consumer redelivery.
  // Returns true if row was inserted OR already existed (both = OK).
  bool InsertOpen(const fob::execution::v1::ExecutionIntent& intent);

  // Update accumulated state from a published ExecutionReport.
  // Increments filled_qty by report.filled_qty, recomputes avg_fill_price
  // (weighted), updates status, error_*, completed_at.
  // No-op if hedge_flow_id row doesn't exist (logs WARN).
  bool ApplyReport(const fob::execution::v1::ExecutionReport& report);

 private:
  std::string connection_string_;
};

}  // namespace cex::venues::infra
