#include "observability_service.hpp"

#include <httplib.h>

#include "transport/mappers/alert.hpp"

namespace cex::observability::transport {

ObservabilityService::ObservabilityService(domain::IAlertSubscriber& subscriber,
                                           std::string clickhouse_url)
    : subscriber_(subscriber), clickhouse_url_(std::move(clickhouse_url)) {}

grpc::Status ObservabilityService::SubscribeAlerts(
    grpc::ServerContext* context, const google::protobuf::Empty*,
    grpc::ServerWriter<fob::observability::v1::Alert>* writer) {
  while (!context->IsCancelled()) {
    auto alert = subscriber_.Consume();
    if (!alert) break;
    writer->Write(mappers::ToProto(*alert));
  }
  return grpc::Status::OK;
}

grpc::Status ObservabilityService::GetQualityMetrics(
    grpc::ServerContext*, const fob::observability::v1::GetQualityMetricsRequest* request,
    fob::observability::v1::GetQualityMetricsResponse* response) {
  // Формируем SQL запрос к ClickHouse
  std::string query =
      "SELECT toUnixTimestamp(createdAt) as ts, epsilon1, epsilon2, epsilon3, confidence, level "
      "FROM venue_liquidity_curves "
      "WHERE venueId = '" +
      request->venue_id() +
      "' "
      "AND symbol = '" +
      request->symbol() +
      "' "
      "AND side = '" +
      request->side() +
      "' "
      "AND createdAt >= fromUnixTimestamp(" +
      std::to_string(request->from().seconds()) +
      ") "
      "AND createdAt <= fromUnixTimestamp(" +
      std::to_string(request->to().seconds()) +
      ") "
      "ORDER BY createdAt "
      "FORMAT JSONEachRow";

  httplib::Client client(clickhouse_url_);
  auto result = client.Post("/", query, "text/plain");

  if (!result || result->status != 200) {
    return {grpc::StatusCode::INTERNAL, "ClickHouse query failed"};
  }

  // Добавляем тестовые q для comparison
  response->mutable_comparison()->add_test_q(0.1);
  response->mutable_comparison()->add_test_q(1.0);
  response->mutable_comparison()->add_test_q(5.0);
  response->mutable_comparison()->add_test_q(10.0);

  return grpc::Status::OK;
}

}  // namespace cex::observability::transport
