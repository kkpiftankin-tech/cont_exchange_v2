// ============================================================================
// grpc_ledger_service.cpp — gRPC transport layer для LedgerService.
//
// Тонкий wrapper над LedgerUseCases (app/ledger_uc.cpp). Бизнес-логика
// (idempotency, FSM, balance invariants — CLAUDE.md §17) живёт в UC.
//
// Группы методов:
//   * User balances:    GetBalances / ReserveFunds / ReleaseFunds
//                       (F-02 reservations, F-04 fills applying).
//   * Fills application: ApplyBatchResult (F-04 / F-09 if grouped).
//   * Execution:        ApplyExecutionReport (F-12 hedge reconciliation).
//   * PnL:              GetUnrealisedPnL / GetRealisedPnL (F-06).
//   * Venue balances:   GetVenueBalances / UpdateVenueBalance (F-12 admin).
//   * Hedge PnL:        RecordHedgeExecution / GetHedgePnL (F-12 DoD-6).
//
// Note: production должен fetch'ить current_prices из market_data сервиса
// для GetUnrealisedPnL — сейчас MVP fallback на avg_entry_price (PnL = 0).
// ============================================================================

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

grpc::Status GrpcLedgerService::GetUnrealisedPnL(
    grpc::ServerContext*,
    const fob::ledger::v1::GetUnrealisedPnLRequest* request,
    fob::ledger::v1::GetUnrealisedPnLResponse* response) {
  // For MVP, pass empty current_prices map (will fallback to avg_entry_price)
  // In production, you would fetch current prices from market data service
  std::unordered_map<std::string, cex::common::Decimal> current_prices;
  
  *response = uc_->GetUnrealisedPnL(*request, current_prices);
  return grpc::Status::OK;
}

grpc::Status GrpcLedgerService::GetRealisedPnL(
    grpc::ServerContext*,
    const fob::ledger::v1::GetRealisedPnLRequest* request,
    fob::ledger::v1::GetRealisedPnLResponse* response) {
  *response = uc_->GetRealisedPnL(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcLedgerService::GetVenueBalances(
    grpc::ServerContext*,
    const fob::ledger::v1::GetVenueBalancesRequest* request,
    fob::ledger::v1::GetVenueBalancesResponse* response) {
  *response = uc_->GetVenueBalances(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcLedgerService::UpdateVenueBalance(
    grpc::ServerContext*,
    const fob::ledger::v1::UpdateVenueBalanceRequest* request,
    fob::ledger::v1::UpdateVenueBalanceResponse* response) {
  *response = uc_->UpdateVenueBalance(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcLedgerService::RecordHedgeExecution(
    grpc::ServerContext*,
    const fob::ledger::v1::RecordHedgeExecutionRequest* request,
    fob::ledger::v1::RecordHedgeExecutionResponse* response) {
  *response = uc_->RecordHedgeExecution(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcLedgerService::GetHedgePnL(
    grpc::ServerContext*,
    const fob::ledger::v1::GetHedgePnLRequest* request,
    fob::ledger::v1::GetHedgePnLResponse* response) {
  *response = uc_->GetHedgePnL(*request);
  return grpc::Status::OK;
}

grpc::Status GrpcLedgerService::GetPositions(
    grpc::ServerContext*,
    const fob::ledger::v1::GetPositionsRequest* request,
    fob::ledger::v1::GetPositionsResponse* response) {
  // MVP: empty mark price map → unrealized_pnl falls back to last persisted
  // value (SEQ-LEDGER-001 stale-mark path). Production fetches mark prices from
  // the Market Data service and passes them here.
  std::unordered_map<std::string, cex::common::Decimal> mark_prices;
  *response = uc_->GetPositionsView(*request, mark_prices);
  return grpc::Status::OK;
}

}  // namespace cex::ledger::transport