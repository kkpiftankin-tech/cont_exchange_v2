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

  // F-06 (T-F06-021): user's open positions with mark-to-market unrealized PnL.
  grpc::Status GetPositions(grpc::ServerContext* context,
                            const fob::ledger::v1::GetPositionsRequest* request,
                            fob::ledger::v1::GetPositionsResponse* response) override;

  // F-18 (ADR-054 §10): валютный вектор биржи (house + venue + client liabilities).
  grpc::Status GetExchangeBalances(grpc::ServerContext* context,
                                   const fob::ledger::v1::GetExchangeBalancesRequest* request,
                                   fob::ledger::v1::GetExchangeBalancesResponse* response) override;

  // F-18 (§11): история позиции биржи по клирингам (старая → Δ → новая).
  grpc::Status GetExchangeNopHistory(grpc::ServerContext* context,
                                     const fob::ledger::v1::GetExchangeNopHistoryRequest* request,
                                     fob::ledger::v1::GetExchangeNopHistoryResponse* response) override;

  grpc::Status GetNodeBalances(grpc::ServerContext* context,
                               const fob::ledger::v1::GetNodeBalancesRequest* request,
                               fob::ledger::v1::GetNodeBalancesResponse* response) override;

  // F-18 v2 (T-F18-203, ADR-061 §7): знаковые накопленные позиции CE-агентов.
  grpc::Status GetAgentPositions(grpc::ServerContext* context,
                                 const fob::ledger::v1::GetAgentPositionsRequest* request,
                                 fob::ledger::v1::GetAgentPositionsResponse* response) override;

  // F-18 v2 (наблюдаемость такта клиринга): per-agent дельты ОДНОГО такта
  // (batch_id) — ДО → Δ → ПОСЛЕ, для вкладки Clearing.
  grpc::Status GetAgentPositionDeltas(
      grpc::ServerContext* context,
      const fob::ledger::v1::GetAgentPositionDeltasRequest* request,
      fob::ledger::v1::GetAgentPositionDeltasResponse* response) override;

 private:
  app::LedgerUseCases* uc_;
};

}  // namespace cex::ledger::transport