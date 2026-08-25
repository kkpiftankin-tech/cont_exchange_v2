#pragma once
// ============================================================================
// grpc_compensation_service.hpp — F-09 MVP-6 slice 3b (T-F09-065, ADR-040).
//
// gRPC-фасад над PostgresComboCompensationRepository. matching остаётся
// единственным владельцем combo_compensations; order_flow вызывает этот сервис
// как client (read + idempotent-resolve) и сам создаёт reversing-FlowOrder.
// Сервис НЕ создаёт ордера и не мутирует ledger (CLAUDE.md §10.3).
// ============================================================================

#include "fob/matching/v1/compensation.grpc.pb.h"

#include "infra/postgres_combo_compensation_repository.hpp"

namespace cex::matching::transport {

class GrpcCompensationService final
    : public fob::matching::v1::CompensationService::Service {
public:
  explicit GrpcCompensationService(infra::PostgresComboCompensationRepository* repo)
      : repo_(repo) {}

  grpc::Status ListPendingCompensations(
      grpc::ServerContext* context,
      const fob::matching::v1::ListPendingCompensationsRequest* request,
      fob::matching::v1::ListPendingCompensationsResponse* response) override;

  grpc::Status GetPendingCompensation(
      grpc::ServerContext* context,
      const fob::matching::v1::GetPendingCompensationRequest* request,
      fob::matching::v1::GetPendingCompensationResponse* response) override;

  grpc::Status ResolvePending(
      grpc::ServerContext* context,
      const fob::matching::v1::ResolvePendingRequest* request,
      fob::matching::v1::ResolvePendingResponse* response) override;

private:
  infra::PostgresComboCompensationRepository* repo_;
};

}  // namespace cex::matching::transport
