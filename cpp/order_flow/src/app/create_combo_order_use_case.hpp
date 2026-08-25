#pragma once
// ============================================================================
// create_combo_order_use_case.hpp — F-09 (T-F09-031).
//
// Application use case приёма ComboOrder: proto → domain нормализация →
// validate (инварианты + ADR-031) → risk check (hook; реальный Risk — фаза E) →
// persist (IComboOrderRepository) → publish (orchestration_only ноги в
// orders.normalized). Идемпотентность по client_combo_id (in-memory, MVP-1).
// ============================================================================

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "domain/combo_order.hpp"
#include "domain/combo_policy.hpp"
#include "fob/orders/v1/combo.pb.h"
#include "infra/orders_normalized_grouped_producer.hpp"
#include "infra/postgres_combo_order_repository.hpp"

namespace cex::order_flow::app {

class CreateComboOrderUseCase {
 public:
  /// Pre-trade групповой risk-check. Возвращает true=approve. На reject
  /// заполняет reject_reason. В MVP-1 — заглушка (approve); реальный Risk
  /// (RiskService/PreTradeCheckGroup) — фаза E (T-F09-040).
  using RiskCheckFn = std::function<bool(const domain::ComboOrder&, std::string& reject_reason)>;

  CreateComboOrderUseCase(infra::IComboOrderRepository& repository,
                          infra::OrdersNormalizedGroupedProducer& producer,
                          RiskCheckFn risk_check,
                          // F-09 (T-F09-002): feature flags + лимиты. По умолчанию
                          // разрешающая — поведение прежних вызовов не меняется.
                          domain::ComboPolicy policy = domain::ComboPolicy::Permissive());

  fob::orders::v1::CreateComboOrderResponse Execute(
      const fob::orders::v1::CreateComboOrderRequest& req);

 private:
  infra::IComboOrderRepository& repository_;
  infra::OrdersNormalizedGroupedProducer& producer_;
  RiskCheckFn risk_check_;
  domain::ComboPolicy policy_;

  std::mutex idempotency_mu_;
  std::unordered_map<std::string, std::string> idempotency_;  // client_combo_id → combo_id
};

}  // namespace cex::order_flow::app
