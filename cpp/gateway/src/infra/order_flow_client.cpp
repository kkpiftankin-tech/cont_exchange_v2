#include "infra/order_flow_client.hpp"

#include "cex/common/log.hpp"

namespace cex::gateway::infra {

OrderFlowClient::OrderFlowClient(const std::string& target) {
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  stub_ = fob::orders::v1::OrderFlowService::NewStub(channel);
  cex::common::log_json("INFO", "OrderFlowClient created", {{"target", target}});
}

fob::orders::v1::CreateFlowOrderResponse OrderFlowClient::CreateFlowOrder(
    const fob::orders::v1::CreateFlowOrderRequest& req) {
  fob::orders::v1::CreateFlowOrderResponse resp;
  grpc::ClientContext ctx;
  auto status = stub_->CreateFlowOrder(&ctx, req, &resp);
  if (!status.ok()) {
    cex::common::log_json("ERROR", "CreateFlowOrder gRPC failed",
                          {{"code", std::to_string(status.error_code())},
                           {"msg", status.error_message()}});
    resp.set_accepted(false);
    auto* err = resp.mutable_error();
    err->set_code("GRPC_ERROR");
    err->set_message(status.error_message());
  }
  return resp;
}

}  // namespace cex::gateway::infra
