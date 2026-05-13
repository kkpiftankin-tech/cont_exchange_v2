#pragma once
// =============================================================================
// gRPC-обёртка для OrderFlowService. Никакой логики — только проксирование
// в use-case'ы. Бизнес-ошибки уходят клиенту в payload (accepted=false и т.п.).
// =============================================================================

#include "fob/orders/v1/order_flow_service.grpc.pb.h"
#include "app/order_flow_uc.hpp"

namespace cex::order_flow::transport {

class GrpcOrderFlowService final : public fob::orders::v1::OrderFlowService::Service {
 public:
  // uc живёт в main; здесь только указатель.
  explicit GrpcOrderFlowService(app::OrderFlowUseCases* uc) : uc_(uc) {}

  // Создание flow-ордера: см. OrderFlowUseCases::CreateFlowOrder.
  grpc::Status CreateFlowOrder(grpc::ServerContext* context,
                              const fob::orders::v1::CreateFlowOrderRequest* request,
                              fob::orders::v1::CreateFlowOrderResponse* response) override;

  // Отмена ордера. См. OrderFlowUseCases::CancelFlowOrder.
  grpc::Status CancelFlowOrder(grpc::ServerContext* context,
                               const fob::orders::v1::CancelFlowOrderRequest* request,
                               fob::orders::v1::CancelFlowOrderResponse* response) override;

  // Чтение ордера из локального кэша.
  grpc::Status GetFlowOrder(grpc::ServerContext* context,
                            const fob::orders::v1::GetFlowOrderRequest* request,
                            fob::orders::v1::GetFlowOrderResponse* response) override;

 private:
  app::OrderFlowUseCases* uc_;
};

}  // namespace cex::order_flow::transport
