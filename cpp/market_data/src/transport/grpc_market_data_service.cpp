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

}  // namespace cex::market_data::transport
