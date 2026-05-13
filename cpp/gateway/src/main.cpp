// =============================================================================
// Entrypoint сервиса gateway. Поднимает HTTP-сервер (Crow), который
// конвертирует REST-запросы в proto и шлёт их в order_flow по gRPC.
// =============================================================================

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "infra/order_flow_client.hpp"
#include "transport/http_gateway.hpp"

int main() {
  // Адрес order_flow в кластере (по умолчанию resolve по имени сервиса в docker-compose).
  const std::string order_flow_addr =
      cex::common::Env::get_string("ORDER_FLOW_GRPC_ADDR", "order_flow:50051");

  // Порт HTTP-сервера: по умолчанию 8080 — отдаётся клиентам/UI.
  const int port = cex::common::Env::get_int("GATEWAY_HTTP_PORT", 8080);

  cex::gateway::infra::OrderFlowClient client(order_flow_addr);
  cex::gateway::transport::HttpGateway gw(std::move(client));

  // Блокирующий запуск Crow на указанном порту.
  gw.run(static_cast<uint16_t>(port));
  return 0;
}
