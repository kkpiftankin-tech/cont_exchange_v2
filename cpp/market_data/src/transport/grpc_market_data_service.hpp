#pragma once

#include "fob/marketdata/v1/marketdata_raw.grpc.pb.h"
#include "fob/marketdata/v1/bbo.pb.h"

#include "app/market_data_uc.hpp"
#include "domain/ports/i_order_book_subscriber.hpp"
#include "infra/order_book_channel.hpp" 

namespace cex::market_data::transport {

class GrpcMarketDataService final : public fob::marketdata::v1::MarketDataService::Service {
 public:
  explicit GrpcMarketDataService(app::MarketDataUseCases *uc,
                                 domain::IOrderBookSubscriber *subscriber)
      : uc_(uc), subscriber_(subscriber) {}

  grpc::Status GetLastTicker(grpc::ServerContext *context,
                             const fob::marketdata::v1::GetLastTickerRequest *request,
                             fob::marketdata::v1::GetLastTickerResponse *response) override;

  grpc::Status GetLiveMarketSnapshots(
      grpc::ServerContext *context, const google::protobuf::Empty *request,
      grpc::ServerWriter<fob::marketdata::v1::OrderBookSnapshot> *writer) override;

  grpc::Status GetLiquidityCurve(
      grpc::ServerContext *context,
      const fob::marketdata::v1::GetLiquidityCurveRequest *request,
      fob::marketdata::v1::GetLiquidityCurveResponse *response) override;

  grpc::Status SubscribeBBO(
      grpc::ServerContext *context,
      const google::protobuf::Empty *request,
      grpc::ServerWriter<fob::marketdata::v1::BBOUpdate> *writer) override;

 private:
  app::MarketDataUseCases *uc_;
  domain::IOrderBookSubscriber *subscriber_;
};

}  // namespace cex::market_data::transport