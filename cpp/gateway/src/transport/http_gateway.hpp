#pragma once
#include <cstdint>

#include "infra/order_flow_client.hpp"

namespace cex::gateway::transport {

// REST/HTTP edge gateway.
// Only responsibility: translate HTTP/JSON <-> gRPC contracts, auth, rate limiting.
// Business logic lives in OrderFlow service (per methodology).
class HttpGateway {
 public:
  explicit HttpGateway(infra::OrderFlowClient client);

  void run(uint16_t port);

 private:
  infra::OrderFlowClient order_flow_;
};

}  // namespace cex::gateway::transport
