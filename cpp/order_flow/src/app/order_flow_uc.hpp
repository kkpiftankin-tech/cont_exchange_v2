#pragma once
#include <mutex>
#include <unordered_map>

#include "cex/common/decimal.hpp"
#include "fob/matching/v1/batch.pb.h"
#include "fob/orders/v1/order_flow_service.pb.h"

#include "infra/risk_client.hpp"
#include "infra/ledger_client.hpp"
#include "infra/orders_kafka_publisher.hpp"

namespace cex::order_flow::app {

// Use-case layer: orchestrates Risk + Ledger + Kafka events.
// This is the "application layer" described in methodology.
class OrderFlowUseCases {
 public:
  OrderFlowUseCases(infra::RiskClient risk,
                    infra::LedgerClient ledger,
                    infra::OrdersKafkaPublisher publisher);

  fob::orders::v1::CreateFlowOrderResponse CreateFlowOrder(
      const fob::orders::v1::CreateFlowOrderRequest& req);

  fob::orders::v1::CancelFlowOrderResponse CancelFlowOrder(
      const fob::orders::v1::CancelFlowOrderRequest& req);

  fob::orders::v1::GetFlowOrderResponse GetFlowOrder(
      const fob::orders::v1::GetFlowOrderRequest& req);

  fob::orders::v1::ListFlowOrdersResponse ListFlowOrders(
      const fob::orders::v1::ListFlowOrdersRequest& req);

  // Consumes BatchResult events from matching service (Kafka topic
  // `batch.outputs`) and updates the in-memory FlowOrder store so gRPC
  // readers (gateway, UI via frontend-api) see fresh remaining_qty,
  // filled_qty_total, and status. Idempotent: applies each (batch_id,
  // order_id) pair at most once.
  // Safe to call from a background Kafka consumer thread.
  void ApplyBatchResult(const fob::matching::v1::BatchResult& batch);

 private:
  infra::RiskClient risk_;
  infra::LedgerClient ledger_;
  infra::OrdersKafkaPublisher publisher_;

  // Minimal in-memory order store for MVP/dev.
  // orders_mu_ guards both maps; held across single-statement reads/writes only.
  mutable std::mutex orders_mu_;
  std::unordered_map<std::string, fob::orders::v1::FlowOrder> orders_;

  // Idempotency guard for ApplyBatchResult: composite key "batch_id|order_id".
  std::unordered_map<std::string, bool> applied_fills_;
};

}  // namespace cex::order_flow::app
