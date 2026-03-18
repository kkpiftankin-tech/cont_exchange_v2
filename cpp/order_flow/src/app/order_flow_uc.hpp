#pragma once
#include <unordered_map>

#include "cex/common/decimal.hpp"
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

 private:
  infra::RiskClient risk_;
  infra::LedgerClient ledger_;
  infra::OrdersKafkaPublisher publisher_;

  // Minimal in-memory order store for MVP/dev.
  std::unordered_map<std::string, fob::orders::v1::FlowOrder> orders_;
};

}  // namespace cex::order_flow::app
