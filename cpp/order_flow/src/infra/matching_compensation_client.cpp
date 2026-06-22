#include "infra/matching_compensation_client.hpp"

#include "cex/common/log.hpp"

namespace cex::order_flow::infra {

MatchingCompensationClient::MatchingCompensationClient(const std::string& target) {
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  stub_ = fob::matching::v1::CompensationService::NewStub(channel);
  cex::common::log_json("INFO", "MatchingCompensationClient created", {{"target", target}});
}

fob::matching::v1::ListPendingCompensationsResponse MatchingCompensationClient::ListPending(
    const fob::matching::v1::ListPendingCompensationsRequest& req) {
  fob::matching::v1::ListPendingCompensationsResponse resp;
  grpc::ClientContext ctx;
  const auto status = stub_->ListPendingCompensations(&ctx, req, &resp);
  if (!status.ok()) {
    cex::common::log_json("ERROR", "Matching ListPendingCompensations gRPC failed",
                          {{"code", std::to_string(status.error_code())},
                           {"msg", status.error_message()}});
  }
  return resp;  // пустой список при ошибке (авто-loop просто ничего не делает)
}

fob::matching::v1::GetPendingCompensationResponse MatchingCompensationClient::GetPending(
    const fob::matching::v1::GetPendingCompensationRequest& req) {
  fob::matching::v1::GetPendingCompensationResponse resp;
  grpc::ClientContext ctx;
  const auto status = stub_->GetPendingCompensation(&ctx, req, &resp);
  if (!status.ok()) {
    cex::common::log_json("ERROR", "Matching GetPendingCompensation gRPC failed",
                          {{"code", std::to_string(status.error_code())},
                           {"msg", status.error_message()}});
    resp.set_found(false);  // fail-closed: нет данных → не резолвим
  }
  return resp;
}

fob::matching::v1::ResolvePendingResponse MatchingCompensationClient::ResolvePending(
    const fob::matching::v1::ResolvePendingRequest& req) {
  fob::matching::v1::ResolvePendingResponse resp;
  grpc::ClientContext ctx;
  const auto status = stub_->ResolvePending(&ctx, req, &resp);
  if (!status.ok()) {
    cex::common::log_json("ERROR", "Matching ResolvePending gRPC failed",
                          {{"code", std::to_string(status.error_code())},
                           {"msg", status.error_message()}});
    resp.set_applied(false);
    auto* err = resp.mutable_error();
    err->set_code("GRPC_ERROR");
    err->set_message(status.error_message());
  }
  return resp;
}

}  // namespace cex::order_flow::infra
