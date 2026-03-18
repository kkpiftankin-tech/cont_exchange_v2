#pragma once
#include "fob/ledger/v1/ledger.grpc.pb.h"
#include "app/ledger_uc.hpp"

namespace cex::ledger::transport {

class GrpcLedgerService final : public fob::ledger::v1::LedgerService::Service {
 public:
  explicit GrpcLedgerService(app::LedgerUseCases* uc) : uc_(uc) {}

  grpc::Status GetBalances(grpc::ServerContext* context,
                           const fob::ledger::v1::GetBalancesRequest* request,
                           fob::ledger::v1::GetBalancesResponse* response) override;

  grpc::Status ReserveFunds(grpc::ServerContext* context,
                            const fob::ledger::v1::ReserveFundsRequest* request,
                            fob::ledger::v1::ReserveFundsResponse* response) override;

  grpc::Status ReleaseFunds(grpc::ServerContext* context,
                            const fob::ledger::v1::ReleaseFundsRequest* request,
                            google::protobuf::Empty* response) override;

  grpc::Status ApplyBatchResult(grpc::ServerContext* context,
                                const fob::ledger::v1::ApplyBatchResultRequest* request,
                                fob::ledger::v1::ApplyBatchResultResponse* response) override;

  grpc::Status ApplyExecutionReport(grpc::ServerContext* context,
                                    const fob::ledger::v1::ApplyExecutionReportRequest* request,
                                    google::protobuf::Empty* response) override;

 private:
  app::LedgerUseCases* uc_;
};

}  // namespace cex::ledger::transport
