#include "transport/grpc_risk_service.hpp"

namespace cex::risk::transport {

grpc::Status GrpcRiskService::CheckNewOrder(
    grpc::ServerContext*,
    const fob::risk::v1::PreTradeCheckRequest* request,
    fob::risk::v1::PreTradeCheckResponse* response) {
  *response = uc_->CheckNewOrder(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcRiskService::SetKillSwitch(
    grpc::ServerContext*,
    const fob::risk::v1::KillSwitchRequest* request,
    fob::risk::v1::KillSwitchResponse* response) {
  *response = uc_->SetKillSwitch(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcRiskService::OnBatchResult(
    grpc::ServerContext*,
    const fob::risk::v1::PostTradeUpdateRequest* request,
    google::protobuf::Empty*) {
  uc_->OnBatchResult(*request);
  return grpc::Status::OK;
}

}  // namespace cex::risk::transport
