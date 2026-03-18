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

}  // namespace cex::order_flow::transport
