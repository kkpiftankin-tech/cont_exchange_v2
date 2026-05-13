#pragma once
// =============================================================================
// gRPC-сервис MarketData. В MVP единственный метод — GetLastTicker.
// При расширении (история, OHLCV, стрим-подписки) сюда добавляются
// соответствующие RPC, бизнес-логика — в use-cases.
// =============================================================================

#include "fob/marketdata/v1/marketdata_service.grpc.pb.h"
#include "app/market_data_uc.hpp"

namespace cex::market_data::transport {

class GrpcMarketDataService final : public fob::marketdata::v1::MarketDataService::Service {
 public:
  explicit GrpcMarketDataService(app::MarketDataUseCases* uc) : uc_(uc) {}

  // RPC получения последнего тикера. См. .cpp для маппинга nullopt -> NOT_FOUND.
  grpc::Status GetLastTicker(grpc::ServerContext* context,
                             const fob::marketdata::v1::GetLastTickerRequest* request,
                             fob::marketdata::v1::GetLastTickerResponse* response) override;

 private:
  app::MarketDataUseCases* uc_;
};

}  // namespace cex::market_data::transport
