#include "grpc_isolation_matching_service.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "cex/common/log.hpp"
#include "domain/flow_order.hpp"

namespace cex::matching::transport {
namespace {

std::string NormalizeSymbol(std::string value) {
  value.erase(std::remove(value.begin(), value.end(), '/'), value.end());
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

}  // namespace

grpc::Status GrpcIsolationSolverService::Solve(
    grpc::ServerContext* context,
    const fob::matching::v1::BatchRequest* request,
    fob::matching::v1::BatchResult* response) {
  (void)context;
  std::vector<domain::FlowOrder> orders;
  for (const auto& order : request->flow_orders()) {
    orders.push_back(domain::FlowOrder::from_proto(order));
  }
  std::unordered_map<std::string, fob::common::v1::Decimal> reference_prices;
  for (const auto& [symbol, price] : request->reference_prices()) {
    reference_prices.emplace(symbol, price);
  }
  domain::ExternalLiquidityBySymbol external_liquidity;
  for (const auto& curve : request->external_liquidity()) {
    const std::string symbol = curve.instrument().symbol();
    if (symbol.empty()) continue;
    external_liquidity[symbol] = curve;
    const std::string normalized = NormalizeSymbol(symbol);
    if (!normalized.empty()) {
      external_liquidity[normalized] = curve;
    }
  }

  cex::common::log_json(
      "INFO",
      "running isolation_solver",
      {{"batch_id", request->batch_id()},
       {"reference_prices", std::to_string(reference_prices.size())},
       {"external_liquidity", std::to_string(external_liquidity.size())}});

  response->CopyFrom(solver_->Solve(orders, reference_prices, external_liquidity));
  response->set_batch_id(request->batch_id());

  return grpc::Status::OK;
}

}  // namespace cex::matching::transport
