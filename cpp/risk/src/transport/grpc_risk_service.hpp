#pragma once
#include "fob/risk/v1/risk.grpc.pb.h"
#include "app/risk_uc.hpp"

namespace cex::risk::transport {

class GrpcRiskService final : public fob::risk::v1::RiskService::Service {
 public:
  explicit GrpcRiskService(app::RiskUseCases* uc) : uc_(uc) {}

  grpc::Status CheckNewOrder(grpc::ServerContext* context,
                            const fob::risk::v1::PreTradeCheckRequest* request,
                            fob::risk::v1::PreTradeCheckResponse* response) override;

  // F-09 (T-F09-040): grouped pre-trade risk check.
  grpc::Status PreTradeCheckGroup(grpc::ServerContext* context,
                                  const fob::risk::v1::PreTradeCheckGroupRequest* request,
                                  fob::risk::v1::PreTradeCheckGroupResponse* response) override;

  // F-12 DoD-3 (PR-F12-13).
  grpc::Status PreHedgeCheck(grpc::ServerContext* context,
                             const fob::risk::v1::PreHedgeCheckRequest* request,
                             fob::risk::v1::PreHedgeCheckResponse* response) override;

  grpc::Status SetKillSwitch(grpc::ServerContext* context,
                             const fob::risk::v1::KillSwitchRequest* request,
                             fob::risk::v1::KillSwitchResponse* response) override;

  grpc::Status OnBatchResult(grpc::ServerContext* context,
                             const fob::risk::v1::PostTradeUpdateRequest* request,
                             google::protobuf::Empty* response) override;

 private:
  app::RiskUseCases* uc_;
};

}  // namespace cex::risk::transport
