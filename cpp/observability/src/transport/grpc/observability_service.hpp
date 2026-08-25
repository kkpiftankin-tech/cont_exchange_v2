#pragma once

#include "fob/observability/v1/observability.grpc.pb.h"
#include "fob/observability/v1/quality.grpc.pb.h"

#include "domain/ports/i_alert_subscriber.hpp"

namespace cex::observability::transport {

class ObservabilityService final : public fob::observability::v1::LobFobQualityService::Service {
 public:
  explicit ObservabilityService(domain::IAlertSubscriber &subscriber, std::string clickhouse_url);

  grpc::Status SubscribeAlerts(grpc::ServerContext *context, const google::protobuf::Empty *request,
                               grpc::ServerWriter<fob::observability::v1::Alert> *writer) override;

  grpc::Status GetQualityMetrics(
      grpc::ServerContext *context, const fob::observability::v1::GetQualityMetricsRequest *request,
      fob::observability::v1::GetQualityMetricsResponse *response) override;

 private:
  domain::IAlertSubscriber &subscriber_;
  std::string clickhouse_url_;
};

}  // namespace cex::observability::transport