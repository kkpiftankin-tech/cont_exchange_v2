#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "fob/common/v1/common.pb.h"
#include "fob/marketdata/v1/marketdata_raw.grpc.pb.h"
#include "fob/orders/v1/orders.pb.h"
#include "domain/flow_order.hpp"
namespace cex::matching::infra {

class MarketDataClient {
 public:
  MarketDataClient(const std::string& target, std::string venue);

  std::unordered_map<std::string, fob::common::v1::Decimal> GetReferencePrices(
      const std::vector<domain::FlowOrder>& orders);

 private:
  std::string venue_;
  std::unique_ptr<fob::marketdata::v1::MarketDataService::Stub> stub_;
};

}  // namespace cex::matching::infra

