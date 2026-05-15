// =============================================================================
// Реализация GetLastTicker. Возвращаем grpc::Status::OK даже когда тикер не
// найден — клиенту удобнее различать "ответ есть, но found=false" и
// "проблема в транспорте/сервисе".
// =============================================================================

#include "transport/grpc_market_data_service.hpp"

#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

namespace cex::market_data::transport {

grpc::Status GrpcMarketDataService::GetLastTicker(
    grpc::ServerContext*,
    const fob::marketdata::v1::GetLastTickerRequest* request,
    fob::marketdata::v1::GetLastTickerResponse* response) {
  // Пробрасываем мета из запроса; перезаписываем source.
  *response->mutable_meta() = request->meta();
  response->mutable_meta()->set_source("market_data");

  auto t = uc_->GetLastTicker(request->venue(), request->symbol());
  if (!t) {
    // Нет данных по этой паре — возвращаем явный NOT_FOUND внутри payload.
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

// Below: UNIMPLEMENTED stubs for the four RPCs added in marketdata_raw.proto
// when it absorbed MarketDataService (origin/dev). Surface preserved so the
// service class is concrete; full bodies will land alongside cpp/market_data
// updates in a follow-up PR. matching service from origin/dev only calls
// GetLastTicker, so these stubs are sufficient for F-04.

grpc::Status GrpcMarketDataService::GetLiveMarketSnapshots(
    grpc::ServerContext*,
    const google::protobuf::Empty*,
    grpc::ServerWriter<fob::marketdata::v1::OrderBookSnapshot>*) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                      "GetLiveMarketSnapshots not yet implemented in main");
}

grpc::Status GrpcMarketDataService::GetLiquidityCurve(
    grpc::ServerContext*,
    const fob::marketdata::v1::GetLiquidityCurveRequest*,
    fob::marketdata::v1::GetLiquidityCurveResponse*) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                      "GetLiquidityCurve not yet implemented in main");
}

grpc::Status GrpcMarketDataService::SubscribeBBO(
    grpc::ServerContext*,
    const google::protobuf::Empty*,
    grpc::ServerWriter<fob::marketdata::v1::BBOUpdate>*) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                      "SubscribeBBO not yet implemented in main");
}

grpc::Status GrpcMarketDataService::GetHedgePnL(
    grpc::ServerContext*,
    const fob::marketdata::v1::GetHedgePnLRequest*,
    fob::marketdata::v1::GetHedgePnLResponse*) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                      "GetHedgePnL not yet implemented in main");
}

}  // namespace cex::market_data::transport
