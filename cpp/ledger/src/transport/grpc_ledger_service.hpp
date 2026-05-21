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

  grpc::Status GetUnrealisedPnL(grpc::ServerContext* context,
                                const fob::ledger::v1::GetUnrealisedPnLRequest* request,
                                fob::ledger::v1::GetUnrealisedPnLResponse* response) override;

  grpc::Status GetRealisedPnL(grpc::ServerContext* context,
                              const fob::ledger::v1::GetRealisedPnLRequest* request,
                              fob::ledger::v1::GetRealisedPnLResponse* response) override;

  grpc::Status GetVenueBalances(grpc::ServerContext* context,
                                const fob::ledger::v1::GetVenueBalancesRequest* request,
                                fob::ledger::v1::GetVenueBalancesResponse* response) override;

  grpc::Status UpdateVenueBalance(grpc::ServerContext* context,
                                  const fob::ledger::v1::UpdateVenueBalanceRequest* request,
                                  fob::ledger::v1::UpdateVenueBalanceResponse* response) override;

  grpc::Status RecordHedgeExecution(grpc::ServerContext* context,
                                    const fob::ledger::v1::RecordHedgeExecutionRequest* request,
                                    fob::ledger::v1::RecordHedgeExecutionResponse* response) override;

  grpc::Status GetHedgePnL(grpc::ServerContext* context,
                           const fob::ledger::v1::GetHedgePnLRequest* request,
                           fob::ledger::v1::GetHedgePnLResponse* response) override;

 private:
  app::LedgerUseCases* uc_;
};

}  // namespace cex::ledger::transport