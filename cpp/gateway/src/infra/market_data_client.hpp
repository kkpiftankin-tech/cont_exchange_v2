#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fob/marketdata/v1/marketdata_raw.grpc.pb.h"

namespace cex::gateway::infra {

class MarketDataClient {
 public:
  explicit MarketDataClient(const std::string& target);

  // Subscribe to BBO stream
  std::unique_ptr<grpc::ClientReader<fob::marketdata::v1::BBOUpdate>> SubscribeBBO();

  // F-12 HedgePnL query proxied to the market_data service.
  fob::marketdata::v1::GetHedgePnLResponse GetHedgePnL(
      const fob::marketdata::v1::GetHedgePnLRequest& request);

 private:
  std::unique_ptr<fob::marketdata::v1::MarketDataService::Stub> stub_;
};

}  // namespace cex::gateway::infra
