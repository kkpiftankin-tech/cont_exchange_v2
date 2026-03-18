#pragma once
#include "fob/marketdata/v1/marketdata_service.grpc.pb.h"
#include "app/market_data_uc.hpp"

namespace cex::market_data::transport {

class GrpcMarketDataService final : public fob::marketdata::v1::MarketDataService::Service {
 public:
  explicit GrpcMarketDataService(app::MarketDataUseCases* uc) : uc_(uc) {}

  grpc::Status GetLastTicker(grpc::ServerContext* context,
                             const fob::marketdata::v1::GetLastTickerRequest* request,
                             fob::marketdata::v1::GetLastTickerResponse* response) override;

 private:
  app::MarketDataUseCases* uc_;
};

}  // namespace cex::market_data::transport
