#include "grpc_market_data_service.hpp"
#include "mappers/order_book.hpp"

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

}  // namespace cex::market_data::transport