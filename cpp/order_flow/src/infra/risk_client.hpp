#pragma once
// =============================================================================
// RiskClient — синхронный gRPC клиент к сервису risk.
// При сетевой ошибке гарантированно возвращает RISK_DECISION_REJECT —
// то есть "дефолт-deny": если risk недоступен, ордера не принимаются.
// Это безопаснее, чем let-fail: лучше отказать клиенту, чем пропустить
// потенциально опасный заказ без проверки рисков.
// =============================================================================

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fob/risk/v1/risk.grpc.pb.h"

namespace cex::order_flow::infra {

class RiskClient {
 public:
  explicit RiskClient(const std::string& target);

  // Pre-trade проверка нового ордера. Никогда не выбрасывает —
  // все ошибки канала превращает в REJECT с error.code="GRPC_ERROR".
  fob::risk::v1::PreTradeCheckResponse CheckNewOrder(
      const fob::risk::v1::PreTradeCheckRequest& req);

 private:
  std::unique_ptr<fob::risk::v1::RiskService::Stub> stub_;
};

}  // namespace cex::order_flow::infra
