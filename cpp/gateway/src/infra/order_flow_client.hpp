#pragma once
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fob/orders/v1/order_flow_service.grpc.pb.h"

namespace cex::gateway::infra {

// Thin gRPC client used by the HTTP Gateway.
// It maps HTTP JSON -> proto -> gRPC call and back.
class OrderFlowClient {
 public:
  explicit OrderFlowClient(const std::string& target);

  fob::orders::v1::CreateFlowOrderResponse CreateFlowOrder(
      const fob::orders::v1::CreateFlowOrderRequest& req);

 private:
  std::unique_ptr<fob::orders::v1::OrderFlowService::Stub> stub_;
};

}  // namespace cex::gateway::infra
