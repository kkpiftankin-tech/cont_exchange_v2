#pragma once
#include "fob/orders/v1/order_flow_service.grpc.pb.h"
#include "app/cancel_combo_order_use_case.hpp"
#include "app/create_combo_order_use_case.hpp"
#include "app/order_flow_uc.hpp"

namespace cex::order_flow::transport {

class GrpcOrderFlowService final : public fob::orders::v1::OrderFlowService::Service {
 public:
  // F-09 combo UCs опциональны (nullptr, если ORDER_FLOW_POSTGRES_DSN не задан —
  // combo требует PG). При nullptr combo-методы возвращают FAILED_PRECONDITION.
  explicit GrpcOrderFlowService(app::OrderFlowUseCases* uc,
                                app::CreateComboOrderUseCase* create_combo = nullptr,
                                app::CancelComboOrderUseCase* cancel_combo = nullptr)
      : uc_(uc), create_combo_(create_combo), cancel_combo_(cancel_combo) {}

  grpc::Status CreateFlowOrder(grpc::ServerContext* context,
                              const fob::orders::v1::CreateFlowOrderRequest* request,
                              fob::orders::v1::CreateFlowOrderResponse* response) override;

  grpc::Status CancelFlowOrder(grpc::ServerContext* context,
                               const fob::orders::v1::CancelFlowOrderRequest* request,
                               fob::orders::v1::CancelFlowOrderResponse* response) override;

  grpc::Status GetFlowOrder(grpc::ServerContext* context,
                            const fob::orders::v1::GetFlowOrderRequest* request,
                            fob::orders::v1::GetFlowOrderResponse* response) override;

  grpc::Status ListFlowOrders(grpc::ServerContext* context,
                              const fob::orders::v1::ListFlowOrdersRequest* request,
                              fob::orders::v1::ListFlowOrdersResponse* response) override;

  // F-09 (T-F09-036). CreateBatchOrder/PreviewComboOrder/GetComboOrder —
  // MVP-2+ (по умолчанию UNIMPLEMENTED, не переопределены здесь).
  grpc::Status CreateComboOrder(grpc::ServerContext* context,
                                const fob::orders::v1::CreateComboOrderRequest* request,
                                fob::orders::v1::CreateComboOrderResponse* response) override;

  grpc::Status CancelComboOrder(grpc::ServerContext* context,
                                const fob::orders::v1::CancelComboOrderRequest* request,
                                fob::orders::v1::CancelComboOrderResponse* response) override;

 private:
  app::OrderFlowUseCases* uc_;
  app::CreateComboOrderUseCase* create_combo_;
  app::CancelComboOrderUseCase* cancel_combo_;
};

}  // namespace cex::order_flow::transport
