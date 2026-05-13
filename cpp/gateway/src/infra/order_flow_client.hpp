#pragma once
// =============================================================================
// OrderFlowClient — тонкий gRPC-клиент к OrderFlowService для шлюза.
// HTTP-обработчик в HttpGateway формирует proto-запрос и зовёт этот класс.
// При gRPC-ошибке оборачиваем результат в accepted=false / GRPC_ERROR — UI
// получит читаемую ошибку без 500'ок на транспорте.
// =============================================================================

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fob/orders/v1/order_flow_service.grpc.pb.h"

namespace cex::gateway::infra {

class OrderFlowClient {
 public:
  explicit OrderFlowClient(const std::string& target);

  // Создание flow-ордера. Никогда не выбрасывает: ошибки канала маппятся
  // в accepted=false с error.code="GRPC_ERROR".
  fob::orders::v1::CreateFlowOrderResponse CreateFlowOrder(
      const fob::orders::v1::CreateFlowOrderRequest& req);

 private:
  std::unique_ptr<fob::orders::v1::OrderFlowService::Stub> stub_;
};

}  // namespace cex::gateway::infra
