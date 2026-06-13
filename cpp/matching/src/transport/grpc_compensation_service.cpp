#include "transport/grpc_compensation_service.hpp"

#include "cex/common/decimal.hpp"

namespace cex::matching::transport {

namespace {

// Зеркалит infra::PendingCompensation → proto. internal_filled_qty — Decimal (§9).
void ToProto(const infra::PendingCompensation& src,
             fob::matching::v1::PendingCompensation* dst) {
  dst->set_compensation_id(src.compensation_id);
  dst->set_parent_order_id(src.parent_order_id);
  dst->set_leg_id(src.leg_id);
  dst->set_reason(src.reason);
  *dst->mutable_internal_filled_qty() = src.internal_filled_qty.to_proto();
}

}  // namespace

grpc::Status GrpcCompensationService::ListPendingCompensations(
    grpc::ServerContext* /*context*/,
    const fob::matching::v1::ListPendingCompensationsRequest* request,
    fob::matching::v1::ListPendingCompensationsResponse* response) {
  *response->mutable_meta() = request->meta();
  const auto pending = request->parent_order_id().empty()
                           ? repo_->ListPending()
                           : repo_->ListPending(request->parent_order_id());
  for (const auto& c : pending) ToProto(c, response->add_compensations());
  return grpc::Status::OK;
}

grpc::Status GrpcCompensationService::GetPendingCompensation(
    grpc::ServerContext* /*context*/,
    const fob::matching::v1::GetPendingCompensationRequest* request,
    fob::matching::v1::GetPendingCompensationResponse* response) {
  *response->mutable_meta() = request->meta();
  const auto found = repo_->GetPending(request->compensation_id());
  response->set_found(found.has_value());
  if (found) ToProto(*found, response->mutable_compensation());
  return grpc::Status::OK;
}

grpc::Status GrpcCompensationService::ResolvePending(
    grpc::ServerContext* /*context*/,
    const fob::matching::v1::ResolvePendingRequest* request,
    fob::matching::v1::ResolvePendingResponse* response) {
  *response->mutable_meta() = request->meta();

  const std::string& action = request->action();
  if (action != "reverse_internal" && action != "retry_external" && action != "accept") {
    auto* err = response->mutable_error();
    err->set_code("INVALID_ACTION");
    err->set_message("action must be reverse_internal | retry_external | accept");
    err->set_details(action);
    response->set_applied(false);
    return grpc::Status::OK;  // бизнес-ошибка в payload, не транспортная
  }
  if (request->compensation_id().empty() || request->operator_id().empty()) {
    auto* err = response->mutable_error();
    err->set_code("INVALID_ARGUMENT");
    err->set_message("compensation_id and operator_id are required");
    response->set_applied(false);
    return grpc::Status::OK;
  }

  // Идемпотентно по status='pending' (ADR-040 §5): повтор → applied=false, не ошибка.
  const bool applied = repo_->ResolvePending(request->compensation_id(), action,
                                             request->operator_id(),
                                             request->resolving_ref());
  response->set_applied(applied);
  return grpc::Status::OK;
}

}  // namespace cex::matching::transport
