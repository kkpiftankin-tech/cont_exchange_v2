#pragma once
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fob/risk/v1/risk.grpc.pb.h"

namespace cex::gateway::infra {

// Thin gRPC client used by the HTTP Gateway to talk to the Risk service.
// Maps HTTP JSON -> proto -> gRPC call and back. F-06 positions/PnL/margin.
class RiskClient {
 public:
  explicit RiskClient(const std::string& target);

  // F-06: aggregated risk/margin snapshot for an entity (user/account).
  // Returns grpc::Status so the caller can gracefully degrade (margin=null)
  // if risk is unavailable instead of failing the whole request.
  grpc::Status GetRiskSnapshot(const fob::risk::v1::GetRiskSnapshotRequest& req,
                               fob::risk::v1::GetRiskSnapshotResponse* resp);

 private:
  std::unique_ptr<fob::risk::v1::RiskService::Stub> stub_;
};

}  // namespace cex::gateway::infra
