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


// F-05: текущий MarketDataSnapshot
fob::marketdata::v1::GetMarketDataSnapshotResponse MarketDataClient::GetMarketDataSnapshot(
    const std::string& asset) {
  fob::marketdata::v1::GetMarketDataSnapshotRequest req;
  req.set_allocated_meta([]() {
    auto* m = new fob::common::v1::EventMeta();
    m->set_source("gateway");
    return m;
  }());
  req.set_asset(asset);

  fob::marketdata::v1::GetMarketDataSnapshotResponse resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(200));
  const grpc::Status status = stub_->GetMarketDataSnapshot(&ctx, req, &resp);
  if (!status.ok()) {
    resp.set_found(false);
    resp.mutable_error()->set_code("GRPC_ERROR");
    resp.mutable_error()->set_message(status.error_message());
  }
  return resp;
}

// F-05: исторические снимки
fob::marketdata::v1::GetMarketDataHistoryResponse MarketDataClient::GetMarketDataHistory(
    const std::string& asset, int limit, int64_t from_unix, int64_t to_unix) {
  fob::marketdata::v1::GetMarketDataHistoryRequest req;
  req.mutable_meta()->set_source("gateway");
  req.set_asset(asset);
  req.set_limit(static_cast<uint32_t>(limit));
  if (from_unix > 0) req.mutable_from()->set_seconds(from_unix);
  if (to_unix > 0) req.mutable_to()->set_seconds(to_unix);

  fob::marketdata::v1::GetMarketDataHistoryResponse resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
  const grpc::Status status = stub_->GetMarketDataHistory(&ctx, req, &resp);
  if (!status.ok()) {
    resp.mutable_error()->set_code("GRPC_ERROR");
    resp.mutable_error()->set_message(status.error_message());
  }
  return resp;
}

// F-05: reference prices (batch)
fob::marketdata::v1::GetReferencePricesResponse MarketDataClient::GetReferencePrices(
    const std::vector<std::string>& assets) {
  fob::marketdata::v1::GetReferencePricesRequest req;
  req.mutable_meta()->set_source("gateway");
  for (const auto& a : assets) req.add_assets(a);

  fob::marketdata::v1::GetReferencePricesResponse resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(200));
  const grpc::Status status = stub_->GetReferencePrices(&ctx, req, &resp);
  if (!status.ok()) {
    resp.mutable_error()->set_code("GRPC_ERROR");
    resp.mutable_error()->set_message(status.error_message());
  }
  return resp;
}


// F-05: effective spread history
grpc::Status MarketDataClient::GetEffectiveSpreadDirect(
    const fob::marketdata::v1::GetEffectiveSpreadRequest& request,
    fob::marketdata::v1::GetEffectiveSpreadResponse* response) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
  const grpc::Status status = stub_->GetEffectiveSpread(&ctx, request, response);
  if (!status.ok()) {
    response->mutable_error()->set_code("GRPC_ERROR");
    response->mutable_error()->set_message(status.error_message());
  }
  return status;
}

// F-05: открывает gRPC server-stream для подписки на market data
std::unique_ptr<grpc::ClientReader<fob::marketdata::v1::MarketDataStreamEvent>>
MarketDataClient::SubscribeMarketData(const std::string& asset, grpc::ClientContext* ctx) {
  fob::marketdata::v1::SubscribeMarketDataRequest req;
  req.mutable_meta()->set_source("gateway");
  req.set_asset(asset);
  return stub_->SubscribeMarketData(ctx, req);
}

}  // namespace cex::gateway::infra
