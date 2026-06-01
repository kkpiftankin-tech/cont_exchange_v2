#pragma once

#include "fob/orders/v1/orders.pb.h"

namespace cex::order_flow::infra {

// Port: persistence of newly-accepted FlowOrder records.
//
// Implemented by PostgresFlowOrderRepository (writes flow_orders +
// flow_order_legs so the F-04 matching loop's PostgresFlowOrderRepository
// can SELECT them) and by null/no-op implementations for unit tests and
// for the dev fallback when no DSN is configured.
//
// Closes IN-007 gap "order-flow-postgres-write-pending": matching's PG
// reader filters status IN ('active','partially_filled'); writers here MUST
// set status='active' on insert so accepted orders show up in the next
// batch cycle.
class IFlowOrderRepository {
 public:
  virtual ~IFlowOrderRepository() = default;

  // Insert one accepted FlowOrder. Caller has already passed risk + reserve.
  // Idempotent on order_id (ON CONFLICT DO NOTHING) so retries / restarts
  // don't double-insert.
  virtual void InsertFlowOrder(const fob::orders::v1::FlowOrder& order) = 0;
};

}  // namespace cex::order_flow::infra
