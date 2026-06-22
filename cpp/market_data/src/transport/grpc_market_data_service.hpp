#pragma once

#include "fob/marketdata/v1/marketdata_raw.grpc.pb.h"
#include "fob/marketdata/v1/marketdata_service.pb.h"
#include "fob/marketdata/v1/bbo.pb.h"

#include "app/market_data_uc.hpp"
#include "domain/ports/i_order_book_subscriber.hpp"
#include "domain/ports/i_snapshot_storage.hpp"
#include "infra/order_book_channel.hpp"
#include "infra/market_data_stream_hub.hpp"

namespace cex::market_data::transport {

class GrpcMarketDataService final : public fob::marketdata::v1::MarketDataService::Service {
 public:
  explicit GrpcMarketDataService(app::MarketDataUseCases *uc,
                                 domain::IOrderBookSubscriber *subscriber,
                                 infra::MarketDataStreamHub *hub = nullptr,
                                 domain::ISnapshotStorage *snapshot_storage = nullptr,
                                 domain::IEffectiveSpreadStorage *spread_storage = nullptr)
      : uc_(uc), subscriber_(subscriber), hub_(hub),
        snapshot_storage_(snapshot_storage), spread_storage_(spread_storage) {}

  // --- существующие методы ---
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

  // --- F-05 методы ---
  grpc::Status GetMarketDataSnapshot(
      grpc::ServerContext *context,
      const fob::marketdata::v1::GetMarketDataSnapshotRequest *request,
      fob::marketdata::v1::GetMarketDataSnapshotResponse *response) override;

  grpc::Status GetReferencePrices(
      grpc::ServerContext *context,
      const fob::marketdata::v1::GetReferencePricesRequest *request,
      fob::marketdata::v1::GetReferencePricesResponse *response) override;

  grpc::Status GetMarketDataHistory(
      grpc::ServerContext *context,
      const fob::marketdata::v1::GetMarketDataHistoryRequest *request,
      fob::marketdata::v1::GetMarketDataHistoryResponse *response) override;

  grpc::Status GetEffectiveSpread(
      grpc::ServerContext *context,
      const fob::marketdata::v1::GetEffectiveSpreadRequest *request,
      fob::marketdata::v1::GetEffectiveSpreadResponse *response) override;

  // F-05: server-streaming для WebSocket клиентов через gateway
  grpc::Status SubscribeMarketData(
      grpc::ServerContext *context,
      const fob::marketdata::v1::SubscribeMarketDataRequest *request,
      grpc::ServerWriter<fob::marketdata::v1::MarketDataStreamEvent> *writer) override;

 private:
  // Маппинг domain snapshot → proto
  static fob::marketdata::v1::MarketDataSnapshot ToProto(
      const domain::MarketDataSnapshot& snap);

  app::MarketDataUseCases *uc_;
  domain::IOrderBookSubscriber *subscriber_;
  infra::MarketDataStreamHub *hub_{nullptr};
  domain::ISnapshotStorage *snapshot_storage_{nullptr};
  domain::IEffectiveSpreadStorage *spread_storage_{nullptr};
};

}  // namespace cex::market_data::transport