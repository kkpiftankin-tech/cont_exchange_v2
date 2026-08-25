#include "infra/ledger_client.hpp"

#include <chrono>

#include "cex/common/log.hpp"

namespace cex::gateway::infra {

LedgerClient::LedgerClient(const std::string& target) {
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  stub_ = fob::ledger::v1::LedgerService::NewStub(channel);
  cex::common::log_json("INFO", "LedgerClient created", {{"target", target}});
}

grpc::Status LedgerClient::GetPositions(
    const fob::ledger::v1::GetPositionsRequest& req,
    fob::ledger::v1::GetPositionsResponse* resp) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() +
                   std::chrono::milliseconds(500));
  const grpc::Status status = stub_->GetPositions(&ctx, req, resp);
  if (!status.ok()) {
    cex::common::log_json("ERROR", "LedgerClient.GetPositions failed",
                          {{"code", std::to_string(status.error_code())},
                           {"message", status.error_message()}});
  }
  return status;
}

grpc::Status LedgerClient::GetBalances(
    const fob::ledger::v1::GetBalancesRequest& req,
    fob::ledger::v1::GetBalancesResponse* resp) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() +
                   std::chrono::milliseconds(500));
  const grpc::Status status = stub_->GetBalances(&ctx, req, resp);
  if (!status.ok()) {
    cex::common::log_json("ERROR", "LedgerClient.GetBalances failed",
                          {{"code", std::to_string(status.error_code())},
                           {"message", status.error_message()}});
  }
  return status;
}

}  // namespace cex::gateway::infra
