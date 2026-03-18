#include "transport/grpc_ledger_service.hpp"

namespace cex::ledger::transport {

grpc::Status GrpcLedgerService::GetBalances(
    grpc::ServerContext*,
    const fob::ledger::v1::GetBalancesRequest* request,
    fob::ledger::v1::GetBalancesResponse* response) {
  *response = uc_->GetBalances(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcLedgerService::ReserveFunds(
    grpc::ServerContext*,
    const fob::ledger::v1::ReserveFundsRequest* request,
    fob::ledger::v1::ReserveFundsResponse* response) {
  *response = uc_->ReserveFunds(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcLedgerService::ReleaseFunds(
    grpc::ServerContext*,
    const fob::ledger::v1::ReleaseFundsRequest* request,
    google::protobuf::Empty*) {
  uc_->ReleaseFunds(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcLedgerService::ApplyBatchResult(
    grpc::ServerContext*,
    const fob::ledger::v1::ApplyBatchResultRequest* request,
    fob::ledger::v1::ApplyBatchResultResponse* response) {
  *response = uc_->ApplyBatchResult(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcLedgerService::ApplyExecutionReport(
    grpc::ServerContext*,
    const fob::ledger::v1::ApplyExecutionReportRequest* request,
    google::protobuf::Empty*) {
  uc_->ApplyExecutionReport(*request);
  return grpc::Status::OK;
}

}  // namespace cex::ledger::transport
