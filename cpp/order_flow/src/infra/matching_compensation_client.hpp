#pragma once
// ============================================================================
// matching_compensation_client.hpp — F-09 MVP-6 slice 3b (T-F09-067, ADR-040).
//
// gRPC-клиент order_flow → matching.CompensationService. matching владеет
// combo_compensations; order_flow читает pending (GetPending) и фиксирует резолв
// (ResolvePending) после создания reversing-FlowOrder. Паттерн RiskClient.
// ============================================================================

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fob/matching/v1/compensation.grpc.pb.h"

namespace cex::order_flow::infra {

class MatchingCompensationClient {
 public:
  explicit MatchingCompensationClient(const std::string& target);

  // Деталь pending-компенсации по id (found=false если нет pending).
  fob::matching::v1::GetPendingCompensationResponse GetPending(
      const fob::matching::v1::GetPendingCompensationRequest& req);

  // Идемпотентный переход pending→resolved|cancelled (applied=false на повтор).
  fob::matching::v1::ResolvePendingResponse ResolvePending(
      const fob::matching::v1::ResolvePendingRequest& req);

 private:
  std::unique_ptr<fob::matching::v1::CompensationService::Stub> stub_;
};

}  // namespace cex::order_flow::infra
