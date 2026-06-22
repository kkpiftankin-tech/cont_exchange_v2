#include "grpc_market_data_service.hpp"
#include "mappers/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace cex::market_data::transport {

grpc::Status GrpcMarketDataService::GetLastTicker(
    grpc::ServerContext*, const fob::marketdata::v1::GetLastTickerRequest* request,
    fob::marketdata::v1::GetLastTickerResponse* response) {
  *response->mutable_meta() = request->meta();
  response->mutable_meta()->set_source("market_data");

  auto t = uc_->GetLastTicker(request->venue(), request->symbol());
  if (!t) {
    response->set_found(false);
    auto* e = response->mutable_error();
    e->set_code("NOT_FOUND");
    e->set_message("No ticker for this venue/symbol yet");
    return grpc::Status::OK;
  }
  response->set_found(true);
  *response->mutable_ticker() = *t;
  return grpc::Status::OK;
}

grpc::Status GrpcMarketDataService::GetLiveMarketSnapshots(
    grpc::ServerContext* context, const google::protobuf::Empty*,
    grpc::ServerWriter<fob::marketdata::v1::OrderBookSnapshot>* writer) {
  while (!context->IsCancelled()) {
    auto book = subscriber_->Consume();
    if (!book) break;
    writer->Write(mappers::ToProto(*book));
  }
  return grpc::Status::OK;
}

grpc::Status GrpcMarketDataService::GetLiquidityCurve(
    grpc::ServerContext*,
    const fob::marketdata::v1::GetLiquidityCurveRequest* request,
    fob::marketdata::v1::GetLiquidityCurveResponse* response) {
  
  *response->mutable_meta() = request->meta();
  response->mutable_meta()->set_source("market_data");

  auto curve = uc_->GetLiquidityCurve(request->venue(), request->symbol(), request->side());
  
  if (!curve) {
    response->set_found(false);
    auto* e = response->mutable_error();
    e->set_code("NOT_FOUND");
    e->set_message("No liquidity curve for this venue/symbol/side");
    return grpc::Status::OK;
  }
  
  response->set_found(true);
  *response->mutable_curve() = *curve;
  return grpc::Status::OK;
}

grpc::Status GrpcMarketDataService::SubscribeBBO(
    grpc::ServerContext* context,
    const google::protobuf::Empty*,
    grpc::ServerWriter<fob::marketdata::v1::BBOUpdate>* writer) {
  
  // Cast to OrderBookChannel to access BBO queue
  auto* bbo_channel = dynamic_cast<infra::OrderBookChannel*>(subscriber_);
  if (!bbo_channel) {
    return grpc::Status(grpc::StatusCode::INTERNAL, 
                        "Subscriber is not OrderBookChannel - BBO streaming not available");
  }
  
  while (!context->IsCancelled()) {
    auto bbo_msg = bbo_channel->ConsumeBBO();
    if (!bbo_msg) break;
    
    fob::marketdata::v1::BBOUpdate proto;
    
    // Set venue
    proto.set_venue("internal");
    
    // Set instrument
    auto* instrument = proto.mutable_instrument();
    instrument->set_symbol(bbo_msg->symbol);
    
    // Set timestamp (convert from chrono to protobuf Timestamp)
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        bbo_msg->timestamp.time_since_epoch());
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        bbo_msg->timestamp.time_since_epoch() - seconds);
    proto.mutable_timestamp()->set_seconds(seconds.count());
    proto.mutable_timestamp()->set_nanos(static_cast<int32_t>(nanos.count()));
    
    proto.set_nonce(bbo_msg->nonce);
    
    // Convert double to Decimal (scale = 8 for high precision)
    const int32_t kDecimalScale = 8;
    const double kScaleMultiplier = std::pow(10.0, kDecimalScale);
    
    auto doubleToDecimal = [kScaleMultiplier, kDecimalScale](double value) {
      fob::common::v1::Decimal decimal;
      decimal.set_units(static_cast<int64_t>(value * kScaleMultiplier));
      decimal.set_scale(kDecimalScale);
      return decimal;
    };
    
    // BBO prices and volumes
    *proto.mutable_bid_price() = doubleToDecimal(bbo_msg->bbo.bid_price);
    *proto.mutable_bid_volume() = doubleToDecimal(bbo_msg->bbo.bid_volume);
    *proto.mutable_ask_price() = doubleToDecimal(bbo_msg->bbo.ask_price);
    *proto.mutable_ask_volume() = doubleToDecimal(bbo_msg->bbo.ask_volume);
    
    // Derived metrics
    *proto.mutable_mid_price() = doubleToDecimal(bbo_msg->bbo.mid_price);
    *proto.mutable_spread_absolute() = doubleToDecimal(bbo_msg->bbo.spread_absolute);
    proto.set_spread_relative_percent(bbo_msg->bbo.spread_relative);
    *proto.mutable_microprice() = doubleToDecimal(bbo_msg->bbo.microprice);
    
    // Source info
    proto.set_source("order_book");
    
    writer->Write(proto);
  }
  
  return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// F-05: новые gRPC методы
// ---------------------------------------------------------------------------

fob::marketdata::v1::MarketDataSnapshot GrpcMarketDataService::ToProto(
    const domain::MarketDataSnapshot& snap) {
  fob::marketdata::v1::MarketDataSnapshot proto;
  proto.set_snapshot_id(snap.snapshot_id);
  proto.set_asset(snap.asset);
  proto.mutable_mid()->CopyFrom(snap.mid.to_proto());
  proto.mutable_best_bid()->CopyFrom(snap.best_bid.to_proto());
  proto.mutable_best_ask()->CopyFrom(snap.best_ask.to_proto());
  proto.mutable_spread()->CopyFrom(snap.spread.to_proto());
  proto.mutable_spread_bps()->CopyFrom(snap.spread_bps.to_proto());
  proto.mutable_volume_24h()->CopyFrom(snap.volume_24h.to_proto());
  proto.mutable_clear_price()->CopyFrom(snap.clear_price.to_proto());
  proto.mutable_executed_rate()->CopyFrom(snap.executed_rate.to_proto());
  proto.set_batch_id(snap.batch_id);
  proto.set_stale(snap.stale);
  switch (snap.source) {
    case domain::DataSource::Cex:       proto.set_source("cex");       break;
    case domain::DataSource::Dex:       proto.set_source("dex");       break;
    case domain::DataSource::Composite: proto.set_source("composite"); break;
    default:                            proto.set_source("internal");  break;
  }
  for (const auto& dl : snap.bid_depth) {
    auto* level = proto.add_bid_depth();
    level->mutable_price()->CopyFrom(dl.price.to_proto());
    level->mutable_qty()->CopyFrom(dl.qty.to_proto());
  }
  for (const auto& dl : snap.ask_depth) {
    auto* level = proto.add_ask_depth();
    level->mutable_price()->CopyFrom(dl.price.to_proto());
    level->mutable_qty()->CopyFrom(dl.qty.to_proto());
  }
  return proto;
}

grpc::Status GrpcMarketDataService::GetMarketDataSnapshot(
    grpc::ServerContext*,
    const fob::marketdata::v1::GetMarketDataSnapshotRequest* request,
    fob::marketdata::v1::GetMarketDataSnapshotResponse* response) {
  *response->mutable_meta() = request->meta();
  response->mutable_meta()->set_source("market_data");

  auto snap = uc_->GetMarketDataSnapshot(request->asset());
  if (!snap.has_value()) {
    response->set_found(false);
    response->mutable_error()->set_code("NOT_FOUND");
    response->mutable_error()->set_message("No snapshot for asset: " + request->asset());
    return grpc::Status::OK;
  }

  response->set_found(true);
  *response->mutable_snapshot() = ToProto(*snap);
  return grpc::Status::OK;
}

grpc::Status GrpcMarketDataService::GetReferencePrices(
    grpc::ServerContext*,
    const fob::marketdata::v1::GetReferencePricesRequest* request,
    fob::marketdata::v1::GetReferencePricesResponse* response) {
  *response->mutable_meta() = request->meta();
  response->mutable_meta()->set_source("market_data");

  std::vector<std::string> assets(request->assets().begin(), request->assets().end());
  const auto snapshots = uc_->GetReferencePrices(assets);

  for (const auto& snap : snapshots) {
    auto* rp = response->add_prices();
    rp->set_asset(snap.asset);
    rp->mutable_mid()->CopyFrom(snap.mid.to_proto());
    rp->mutable_best_bid()->CopyFrom(snap.best_bid.to_proto());
    rp->mutable_best_ask()->CopyFrom(snap.best_ask.to_proto());
    switch (snap.source) {
      case domain::DataSource::Cex: rp->set_source("cex"); break;
      case domain::DataSource::Dex: rp->set_source("dex"); break;
      default:                      rp->set_source("internal"); break;
    }
  }
  return grpc::Status::OK;
}

grpc::Status GrpcMarketDataService::GetMarketDataHistory(
    grpc::ServerContext*,
    const fob::marketdata::v1::GetMarketDataHistoryRequest* request,
    fob::marketdata::v1::GetMarketDataHistoryResponse* response) {
  *response->mutable_meta() = request->meta();
  response->mutable_meta()->set_source("market_data");

  if (snapshot_storage_ == nullptr) {
    response->mutable_error()->set_code("UNAVAILABLE");
    response->mutable_error()->set_message("snapshot storage not configured");
    return grpc::Status::OK;
  }

  const uint32_t limit = request->limit() > 0 ? std::min(request->limit(), 1000u) : 100u;
  const auto from_ts = std::chrono::system_clock::from_time_t(
      request->from().seconds());
  const auto to_ts = request->to().seconds() > 0
      ? std::chrono::system_clock::from_time_t(request->to().seconds())
      : std::chrono::system_clock::now();

  const auto snaps = snapshot_storage_->GetSnapshotHistory(
      request->asset(), from_ts, to_ts, limit);

  for (const auto& s : snaps) {
    response->add_snapshots()->CopyFrom(ToProto(s));
  }
  return grpc::Status::OK;
}

grpc::Status GrpcMarketDataService::GetEffectiveSpread(
    grpc::ServerContext*,
    const fob::marketdata::v1::GetEffectiveSpreadRequest* request,
    fob::marketdata::v1::GetEffectiveSpreadResponse* response) {
  *response->mutable_meta() = request->meta();
  response->mutable_meta()->set_source("market_data");

  if (spread_storage_ == nullptr) {
    response->mutable_error()->set_code("UNAVAILABLE");
    response->mutable_error()->set_message("spread storage not configured");
    return grpc::Status::OK;
  }

  const uint32_t limit = request->limit() > 0 ? std::min(request->limit(), 1000u) : 100u;
  const auto from_ts = std::chrono::system_clock::from_time_t(
      request->from().seconds());
  const auto to_ts = request->to().seconds() > 0
      ? std::chrono::system_clock::from_time_t(request->to().seconds())
      : std::chrono::system_clock::now();

  const auto records = spread_storage_->GetSpreadHistory(
      request->asset(), from_ts, to_ts, limit);

  for (const auto& r : records) {
    auto* rec = response->add_records();
    rec->set_fill_id(r.fill_id);
    rec->set_asset(r.asset);
    rec->mutable_exec_price()->CopyFrom(r.exec_price.to_proto());
    rec->mutable_mid_at_exec()->CopyFrom(r.mid_at_exec.to_proto());
    rec->mutable_effective_spread_bps()->CopyFrom(r.effective_spread_bps.to_proto());
    rec->set_batch_id(r.batch_id);
  }
  return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// SubscribeMarketData — server-streaming: snapshot → delta updates
// ---------------------------------------------------------------------------
grpc::Status GrpcMarketDataService::SubscribeMarketData(
    grpc::ServerContext* context,
    const fob::marketdata::v1::SubscribeMarketDataRequest* request,
    grpc::ServerWriter<fob::marketdata::v1::MarketDataStreamEvent>* writer) {
  const std::string asset = request->asset();

  if (hub_ == nullptr) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "stream hub not configured");
  }

  // 1. Отправляем текущий snapshot как первое сообщение
  auto snap = uc_->GetMarketDataSnapshot(asset);
  if (snap.has_value()) {
    fob::marketdata::v1::MarketDataStreamEvent ev;
    *ev.mutable_snapshot() = ToProto(*snap);
    writer->Write(ev);
  }

  // 2. Подписываемся на hub — каждый новый snapshot пишем в стрим
  std::mutex write_mu;
  const auto sub_id = hub_->Subscribe(asset,
      [&](const domain::MarketDataSnapshot& s) {
        if (context->IsCancelled()) return;
        fob::marketdata::v1::MarketDataStreamEvent ev;
        *ev.mutable_snapshot() = ToProto(s);
        std::lock_guard<std::mutex> lg(write_mu);
        writer->Write(ev);
      });

  // 3. Блокируемся до отмены контекста клиентом
  while (!context->IsCancelled()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // 4. Отписываемся
  hub_->Unsubscribe(asset, sub_id);
  return grpc::Status::OK;
}

}  // namespace cex::market_data::transport