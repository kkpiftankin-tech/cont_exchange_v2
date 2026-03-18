#pragma once
#include "fob/orders/v1/order_flow_service.grpc.pb.h"
#include "app/order_flow_uc.hpp"

namespace cex::order_flow::transport {

class GrpcOrderFlowService final : public fob::orders::v1::OrderFlowService::Service {
 public:
  explicit GrpcOrderFlowService(app::OrderFlowUseCases* uc) : uc_(uc) {}

  grpc::Status CreateFlowOrder(grpc::ServerContext* context,
                              const fob::orders::v1::CreateFlowOrderRequest* request,
                              fob::orders::v1::CreateFlowOrderResponse* response) override;

  grpc::Status CancelFlowOrder(grpc::ServerContext* context,
                               const fob::orders::v1::CancelFlowOrderRequest* request,
                               fob::orders::v1::CancelFlowOrderResponse* response) override;

  grpc::Status GetFlowOrder(grpc::ServerContext* context,
                            const fob::orders::v1::GetFlowOrderRequest* request,
                            fob::orders::v1::GetFlowOrderResponse* response) override;

 private:
  app::OrderFlowUseCases* uc_;
};

}  // namespace cex::order_flow::transport
