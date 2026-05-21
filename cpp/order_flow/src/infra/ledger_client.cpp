#include "infra/ledger_client.hpp"

#include "cex/common/log.hpp"

namespace cex::order_flow::infra {

LedgerClient::LedgerClient(const std::string& target) {
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  stub_ = fob::ledger::v1::LedgerService::NewStub(channel);
  cex::common::log_json("INFO", "LedgerClient created", {{"target", target}});
}

fob::ledger::v1::ReserveFundsResponse LedgerClient::ReserveFunds(
    const fob::ledger::v1::ReserveFundsRequest& req) {
  fob::ledger::v1::ReserveFundsResponse resp;
  grpc::ClientContext ctx;
  auto status = stub_->ReserveFunds(&ctx, req, &resp);
  if (!status.ok()) {
    cex::common::log_json("ERROR", "Ledger ReserveFunds gRPC failed",
                          {{"code", std::to_string(status.error_code())},
                           {"msg", status.error_message()}});
    resp.set_success(false);
    auto* err = resp.mutable_error();
    err->set_code("GRPC_ERROR");
    err->set_message(status.error_message());
  }
  return resp;
}

void LedgerClient::ReleaseFunds(const fob::ledger::v1::ReleaseFundsRequest& req) {
  google::protobuf::Empty resp;
  grpc::ClientContext ctx;
  auto status = stub_->ReleaseFunds(&ctx, req, &resp);
  if (!status.ok()) {
    cex::common::log_json("ERROR", "Ledger ReleaseFunds gRPC failed",
                          {{"code", std::to_string(status.error_code())},
                           {"msg", status.error_message()}});
  }
}

fob::ledger::v1::GetBalancesResponse LedgerClient::GetBalances(
    const fob::ledger::v1::GetBalancesRequest& req) {
  fob::ledger::v1::GetBalancesResponse resp;
  grpc::ClientContext ctx;
  auto status = stub_->GetBalances(&ctx, req, &resp);
  if (!status.ok()) {
    cex::common::log_json("ERROR", "Ledger GetBalances gRPC failed",
                          {{"code", std::to_string(status.error_code())},
                           {"msg", status.error_message()}});
  }
  return resp;
}

}  // namespace cex::order_flow::infra
