#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fob/marketdata/v1/marketdata_raw.grpc.pb.h"
#include "fob/marketdata/v1/marketdata_service.pb.h"

namespace cex::gateway::infra {

class MarketDataClient {
 public:
  explicit MarketDataClient(const std::string& target);

  // Subscribe to BBO stream
  std::unique_ptr<grpc::ClientReader<fob::marketdata::v1::BBOUpdate>> SubscribeBBO();

  // F-12 HedgePnL query proxied to the market_data service.
  fob::marketdata::v1::GetHedgePnLResponse GetHedgePnL(
      const fob::marketdata::v1::GetHedgePnLRequest& request);

  // F-05: текущий MarketDataSnapshot для asset
  fob::marketdata::v1::GetMarketDataSnapshotResponse GetMarketDataSnapshot(
      const std::string& asset);

  // F-05: исторические снимки (from/to — unix-секунды, 0 = не задано)
  fob::marketdata::v1::GetMarketDataHistoryResponse GetMarketDataHistory(
      const std::string& asset, int limit = 100,
      int64_t from_unix = 0, int64_t to_unix = 0);

  // F-05: reference prices (batch) для Matching Backend / admin tools
  fob::marketdata::v1::GetReferencePricesResponse GetReferencePrices(
      const std::vector<std::string>& assets);

  // F-05: effective spread history
  grpc::Status GetEffectiveSpreadDirect(
      const fob::marketdata::v1::GetEffectiveSpreadRequest& request,
      fob::marketdata::v1::GetEffectiveSpreadResponse* response);

  // F-05: open streaming subscription — caller owns the context and reader.
  // Caller must call context->TryCancel() to stop the stream.
  std::unique_ptr<grpc::ClientReader<fob::marketdata::v1::MarketDataStreamEvent>>
      SubscribeMarketData(const std::string& asset,
                          grpc::ClientContext* ctx);

 private:
  std::unique_ptr<fob::marketdata::v1::MarketDataService::Stub> stub_;
};

}  // namespace cex::gateway::infra
