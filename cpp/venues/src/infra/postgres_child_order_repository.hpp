#pragma once

#include <string>

#include "fob/execution/v1/execution.pb.h"

namespace cex::venues::infra {

// PostgreSQL persistence for `child_orders` (F-12 / IN-008 DoD-4).
// In the MVP single-venue execution path one ExecutionIntent maps to one
// ChildOrder; multi-venue routing in PR-F12-5 will produce multiple rows
// per HedgeFlow.
//
// `child_order_id` is generated from the intent's `client_order_id` (UUID
// already chosen by Execution Planning). UNIQUE INDEX on
// (hedge_flow_id, client_order_id) provides idempotency against retries.
class PostgresChildOrderRepository final {
 public:
  explicit PostgresChildOrderRepository(std::string connection_string);

  bool EnsureSchema();

  // Insert a PENDING row reflecting the intent the moment it enters the
  // execution adapter. ON CONFLICT DO NOTHING for idempotency.
  bool InsertPending(const fob::execution::v1::ExecutionIntent& intent);

  // Update the row referenced by report.client_order_id (preferred) or
  // report.child_order_id with terminal status / fill_qty / avg_price /
  // venue_order_id from the published report.
  bool ApplyReport(const fob::execution::v1::ExecutionReport& report);

 private:
  std::string connection_string_;
};

}  // namespace cex::venues::infra
