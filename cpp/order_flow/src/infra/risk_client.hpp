#pragma once
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fob/risk/v1/risk.grpc.pb.h"

namespace cex::order_flow::infra {

class RiskClient {
 public:
  explicit RiskClient(const std::string& target);

  fob::risk::v1::PreTradeCheckResponse CheckNewOrder(
      const fob::risk::v1::PreTradeCheckRequest& req);

  // F-09 (T-F09-040): групповой pre-trade check.
  fob::risk::v1::PreTradeCheckGroupResponse PreTradeCheckGroup(
      const fob::risk::v1::PreTradeCheckGroupRequest& req);

 private:
  std::unique_ptr<fob::risk::v1::RiskService::Stub> stub_;
};

}  // namespace cex::order_flow::infra
