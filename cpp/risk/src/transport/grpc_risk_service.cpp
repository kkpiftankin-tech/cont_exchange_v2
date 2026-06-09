// ============================================================================
// grpc_risk_service.cpp — gRPC transport layer для RiskService.
//
// Тонкий wrapper: каждый handler делегирует в RiskUseCases (app/risk_uc.cpp),
// никакой бизнес-логики здесь нет (CLAUDE.md §10 layering).
//
// Методы:
//   CheckNewOrder    (F-07 PreTradeCheck)  — pre-trade gate для FlowOrder.
//   PreHedgeCheck    (F-12 DoD-3)           — pre-hedge gate для ExecutionIntent.
//   SetKillSwitch    (F-16 operator action) — global / per-instrument halt.
//   OnBatchResult    (F-08 post-trade)      — post-trade diagnostics consumer.
// ============================================================================

#include "transport/grpc_risk_service.hpp"

namespace cex::risk::transport {

/// F-07. Возвращает RiskDecision: ACCEPT / RESIZE / REJECT / HALT.
grpc::Status GrpcRiskService::CheckNewOrder(
    grpc::ServerContext*,
    const fob::risk::v1::PreTradeCheckRequest* request,
    fob::risk::v1::PreTradeCheckResponse* response) {
  *response = uc_->CheckNewOrder(*request);
  return grpc::Status::OK;
}

/// F-12 DoD-3 (PR-F12-13). 5-step gate перед отправкой hedge в venue:
/// PROVIDER_HALTED → NOTIONAL → EXPOSURE → SLIPPAGE → VENUES.
grpc::Status GrpcRiskService::PreHedgeCheck(
    grpc::ServerContext*,
    const fob::risk::v1::PreHedgeCheckRequest* request,
    fob::risk::v1::PreHedgeCheckResponse* response) {
  *response = uc_->PreHedgeCheck(*request);
  return grpc::Status::OK;
}

/// F-16 operator action. Глобальный или per-instrument halt/resume.
/// Эмиттит CRITICAL/INFO risk.alerts (см. risk_uc.cpp SetKillSwitch).
grpc::Status GrpcRiskService::SetKillSwitch(
    grpc::ServerContext*,
    const fob::risk::v1::KillSwitchRequest* request,
    fob::risk::v1::KillSwitchResponse* response) {
  *response = uc_->SetKillSwitch(*request);
  return grpc::Status::OK;
}

/// F-08 post-trade. Возвращает google.protobuf.Empty — fire-and-forget.
/// Используется legacy callers; production-path — Kafka batch.outputs consumer.
grpc::Status GrpcRiskService::OnBatchResult(
    grpc::ServerContext*,
    const fob::risk::v1::PostTradeUpdateRequest* request,
    google::protobuf::Empty*) {
  uc_->OnBatchResult(*request);
  return grpc::Status::OK;
}

}  // namespace cex::risk::transport
