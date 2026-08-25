#include "infra/risk_client.hpp"

#include <chrono>

#include "cex/common/log.hpp"

namespace cex::gateway::infra {

RiskClient::RiskClient(const std::string& target) {
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  stub_ = fob::risk::v1::RiskService::NewStub(channel);
  cex::common::log_json("INFO", "RiskClient created", {{"target", target}});
}

grpc::Status RiskClient::GetRiskSnapshot(
    const fob::risk::v1::GetRiskSnapshotRequest& req,
    fob::risk::v1::GetRiskSnapshotResponse* resp) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() +
                   std::chrono::milliseconds(500));
  const grpc::Status status = stub_->GetRiskSnapshot(&ctx, req, resp);
  if (!status.ok()) {
    cex::common::log_json("ERROR", "RiskClient.GetRiskSnapshot failed",
                          {{"code", std::to_string(status.error_code())},
                           {"message", status.error_message()}});
  }
  return status;
}

}  // namespace cex::gateway::infra
