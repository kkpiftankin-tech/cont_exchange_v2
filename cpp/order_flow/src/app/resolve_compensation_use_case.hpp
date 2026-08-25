#pragma once
// ============================================================================
// resolve_compensation_use_case.hpp — F-09 MVP-6 slice 3b (T-F09-067, ADR-040).
//
// Operator-driven разрешение pending-компенсации (ADR-039). Оркестрация (ADR-040 §3):
//   get pending (matching) → reverse_internal: load internal legs (own repo) →
//   ComputeReversals (cex::common) → CreateFlowOrder (order_flow pipeline) →
//   matching.ResolvePending(resolving_ref = order ids).
// Деньги идут ТОЛЬКО через CreateFlowOrder (никакого прямого ledger). Зависимости —
// std::function-порты (тестируется без сети/PG; прод биндит на client/repo/uc).
// ============================================================================

#include <functional>

#include "fob/matching/v1/compensation.pb.h"
#include "fob/orders/v1/order_flow_service.pb.h"

#include "infra/combo_reversal_context.hpp"  // ComboReversalContext (без pqxx)

namespace cex::order_flow::app {

class ResolveCompensationUseCase {
 public:
  using GetPendingFn = std::function<fob::matching::v1::GetPendingCompensationResponse(
      const fob::matching::v1::GetPendingCompensationRequest&)>;
  using ResolvePendingFn = std::function<fob::matching::v1::ResolvePendingResponse(
      const fob::matching::v1::ResolvePendingRequest&)>;
  using LoadLegsFn =
      std::function<infra::ComboReversalContext(const std::string& parent_order_id)>;
  using CreateFlowOrderFn = std::function<fob::orders::v1::CreateFlowOrderResponse(
      const fob::orders::v1::CreateFlowOrderRequest&)>;

  ResolveCompensationUseCase(GetPendingFn get_pending, ResolvePendingFn resolve_pending,
                             LoadLegsFn load_legs, CreateFlowOrderFn create_flow_order)
      : get_pending_(std::move(get_pending)),
        resolve_pending_(std::move(resolve_pending)),
        load_legs_(std::move(load_legs)),
        create_flow_order_(std::move(create_flow_order)) {}

  fob::orders::v1::ResolveCompensationResponse Resolve(
      const fob::orders::v1::ResolveCompensationRequest& req);

 private:
  GetPendingFn get_pending_;
  ResolvePendingFn resolve_pending_;
  LoadLegsFn load_legs_;
  CreateFlowOrderFn create_flow_order_;
};

}  // namespace cex::order_flow::app
