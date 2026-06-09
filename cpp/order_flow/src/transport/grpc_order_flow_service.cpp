// ============================================================================
// grpc_order_flow_service.cpp — gRPC transport layer для OrderFlowService.
//
// Тонкий wrapper над OrderFlowUseCases (app/order_flow_uc.cpp) +
// CreateComboOrderUseCase + CancelComboOrderUseCase (F-09).
//
// Не содержит бизнес-логики — только marshalling proto ↔ use cases
// (CLAUDE.md §10 layering).
//
// Combo methods (CreateComboOrder/CancelComboOrder) требуют PG repository
// (combo_orders + combo_legs + combo_constraints + grouped_links + status_log).
// Если ORDER_FLOW_POSTGRES_DSN не задан → FAILED_PRECONDITION.
// ============================================================================

#include "transport/grpc_order_flow_service.hpp"

namespace cex::order_flow::transport {

grpc::Status GrpcOrderFlowService::CreateFlowOrder(
    grpc::ServerContext*,
    const fob::orders::v1::CreateFlowOrderRequest* request,
    fob::orders::v1::CreateFlowOrderResponse* response) {
  *response = uc_->CreateFlowOrder(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcOrderFlowService::CancelFlowOrder(
    grpc::ServerContext*,
    const fob::orders::v1::CancelFlowOrderRequest* request,
    fob::orders::v1::CancelFlowOrderResponse* response) {
  *response = uc_->CancelFlowOrder(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcOrderFlowService::GetFlowOrder(
    grpc::ServerContext*,
    const fob::orders::v1::GetFlowOrderRequest* request,
    fob::orders::v1::GetFlowOrderResponse* response) {
  *response = uc_->GetFlowOrder(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcOrderFlowService::ListFlowOrders(
    grpc::ServerContext*,
    const fob::orders::v1::ListFlowOrdersRequest* request,
    fob::orders::v1::ListFlowOrdersResponse* response) {
  *response = uc_->ListFlowOrders(*request);
  return grpc::Status::OK;
}

// --- F-09 combo methods (T-F09-036) -----------------------------------------
// FAILED_PRECONDITION при отсутствии PG DSN — без PG нельзя записать combo
// orders (parent + legs + constraints в 5 таблиц атомарно).
// ---------------------------------------------------------------------------

grpc::Status GrpcOrderFlowService::CreateComboOrder(
    grpc::ServerContext*,
    const fob::orders::v1::CreateComboOrderRequest* request,
    fob::orders::v1::CreateComboOrderResponse* response) {
  if (create_combo_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "combo orders require ORDER_FLOW_POSTGRES_DSN");
  }
  *response = create_combo_->Execute(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcOrderFlowService::CancelComboOrder(
    grpc::ServerContext*,
    const fob::orders::v1::CancelComboOrderRequest* request,
    fob::orders::v1::CancelComboOrderResponse* response) {
  if (cancel_combo_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "combo orders require ORDER_FLOW_POSTGRES_DSN");
  }
  *response = cancel_combo_->Execute(*request);
  return grpc::Status::OK;
}

}  // namespace cex::order_flow::transport
