#pragma once
// =============================================================================
// HttpGateway — REST/HTTP-edge биржи.
//
// Единственная ответственность: трансляция HTTP/JSON <-> gRPC/proto + auth +
// rate limiting. Бизнес-логика остаётся в order_flow и других сервисах.
// HTTP-сервер построен на header-only Crow, многопоточный.
// =============================================================================

#include <cstdint>

#include "infra/order_flow_client.hpp"

namespace cex::gateway::transport {

class HttpGateway {
 public:
  // client владеется паблишером — переезжает в gateway по move.
  explicit HttpGateway(infra::OrderFlowClient client);

  // Блокирующий запуск Crow на указанном порту.
  // Возврат происходит только при остановке процесса.
  void run(uint16_t port);

 private:
  infra::OrderFlowClient order_flow_;
};

}  // namespace cex::gateway::transport
