#pragma once
// gRPC MarketDataService. Implements GetLastTicker (real); the other four
// RPCs (GetLiveMarketSnapshots, GetLiquidityCurve, SubscribeBBO, GetHedgePnL)
// are present in the proto contract (consumed by the matching service from
// origin/dev) but not yet wired here in main; they return UNIMPLEMENTED so
// the service class is concrete and the binary links. Full implementations
// land alongside cpp/market_data updates in a follow-up PR.

#include "fob/marketdata/v1/marketdata_raw.grpc.pb.h"
#include "app/market_data_uc.hpp"

namespace cex::market_data::transport {

class GrpcMarketDataService final : public fob::marketdata::v1::MarketDataService::Service {
 public:
  explicit GrpcMarketDataService(app::MarketDataUseCases* uc) : uc_(uc) {}

  // Implemented: returns latest ticker for venue/symbol or NOT_FOUND.
  grpc::Status GetLastTicker(grpc::ServerContext* context,
                             const fob::marketdata::v1::GetLastTickerRequest* request,
                             fob::marketdata::v1::GetLastTickerResponse* response) override;

  // Stubs (UNIMPLEMENTED). Surface kept to satisfy the new proto interface;
  // bodies live in .cpp.
  grpc::Status GetLiveMarketSnapshots(
      grpc::ServerContext* context,
      const google::protobuf::Empty* request,
      grpc::ServerWriter<fob::marketdata::v1::OrderBookSnapshot>* writer) override;

  grpc::Status GetLiquidityCurve(
      grpc::ServerContext* context,
      const fob::marketdata::v1::GetLiquidityCurveRequest* request,
      fob::marketdata::v1::GetLiquidityCurveResponse* response) override;

  grpc::Status SubscribeBBO(
      grpc::ServerContext* context,
      const google::protobuf::Empty* request,
      grpc::ServerWriter<fob::marketdata::v1::BBOUpdate>* writer) override;

  grpc::Status GetHedgePnL(
      grpc::ServerContext* context,
      const fob::marketdata::v1::GetHedgePnLRequest* request,
      fob::marketdata::v1::GetHedgePnLResponse* response) override;

 private:
  app::MarketDataUseCases* uc_;
};

}  // namespace cex::market_data::transport
