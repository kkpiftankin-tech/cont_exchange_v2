#pragma once
// =============================================================================
// gRPC-обёртка над RiskUseCases. Просто проксирует вызовы.
// =============================================================================

#include "fob/risk/v1/risk.grpc.pb.h"
#include "app/risk_uc.hpp"

namespace cex::risk::transport {

class GrpcRiskService final : public fob::risk::v1::RiskService::Service {
 public:
  explicit GrpcRiskService(app::RiskUseCases* uc) : uc_(uc) {}

  // Pre-trade проверка ордера.
  grpc::Status CheckNewOrder(grpc::ServerContext* context,
                            const fob::risk::v1::PreTradeCheckRequest* request,
                            fob::risk::v1::PreTradeCheckResponse* response) override;

  // Включение/выключение торгов (kill switch).
  grpc::Status SetKillSwitch(grpc::ServerContext* context,
                             const fob::risk::v1::KillSwitchRequest* request,
                             fob::risk::v1::KillSwitchResponse* response) override;

  // Post-trade callback от matching/ledger.
  grpc::Status OnBatchResult(grpc::ServerContext* context,
                             const fob::risk::v1::PostTradeUpdateRequest* request,
                             google::protobuf::Empty* response) override;

 private:
  app::RiskUseCases* uc_;
};

}  // namespace cex::risk::transport
