// =============================================================================
// Реализация RiskClient. Все сетевые ошибки -> REJECT (fail-safe).
// =============================================================================

#include "infra/risk_client.hpp"

#include "cex/common/log.hpp"

namespace cex::order_flow::infra {

RiskClient::RiskClient(const std::string& target) {
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  stub_ = fob::risk::v1::RiskService::NewStub(channel);
  cex::common::log_json("INFO", "RiskClient created", {{"target", target}});
}

fob::risk::v1::PreTradeCheckResponse RiskClient::CheckNewOrder(
    const fob::risk::v1::PreTradeCheckRequest& req) {
  fob::risk::v1::PreTradeCheckResponse resp;
  grpc::ClientContext ctx;
  auto status = stub_->CheckNewOrder(&ctx, req, &resp);
  if (!status.ok()) {
    // Default-deny: при сетевой ошибке отправляем явный REJECT с
    // GRPC_ERROR — это позволит UI показать понятную ошибку.
    cex::common::log_json("ERROR", "Risk CheckNewOrder gRPC failed",
                          {{"code", std::to_string(status.error_code())},
                           {"msg", status.error_message()}});
    resp.set_decision(fob::risk::v1::RISK_DECISION_REJECT);
    auto* err = resp.mutable_error();
    err->set_code("GRPC_ERROR");
    err->set_message(status.error_message());
  }
  return resp;
}

}  // namespace cex::order_flow::infra
