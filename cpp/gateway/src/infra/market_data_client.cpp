#include "infra/market_data_client.hpp"

#include "cex/common/log.hpp"

namespace cex::gateway::infra {

MarketDataClient::MarketDataClient(const std::string& target) {
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  stub_ = fob::marketdata::v1::MarketDataService::NewStub(channel);
  cex::common::log_json("INFO", "MarketDataClient created", {{"target", target}});
}

std::unique_ptr<grpc::ClientReader<fob::marketdata::v1::BBOUpdate>> MarketDataClient::SubscribeBBO() {
  google::protobuf::Empty request;
  grpc::ClientContext ctx;
  return stub_->SubscribeBBO(&ctx, request);
}

fob::marketdata::v1::GetHedgePnLResponse MarketDataClient::GetHedgePnL(
    const fob::marketdata::v1::GetHedgePnLRequest& request) {
  fob::marketdata::v1::GetHedgePnLResponse response;
  grpc::ClientContext ctx;
  const grpc::Status status = stub_->GetHedgePnL(&ctx, request, &response);
  if (!status.ok()) {
    response.mutable_error()->set_code("GRPC_ERROR");
    response.mutable_error()->set_message(status.error_message());
    cex::common::log_json("ERROR", "MarketDataClient.GetHedgePnL failed",
                          {{"code", std::to_string(status.error_code())},
                           {"message", status.error_message()}});
  }
  return response;
}

}  // namespace cex::gateway::infra
