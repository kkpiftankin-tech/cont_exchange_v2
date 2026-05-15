#include "grpc_isolation_matching_service.hpp"

#include <unordered_map>

#include "cex/common/log.hpp"
#include "domain/flow_order.hpp"

namespace cex::matching::transport {

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

  cex::common::log_json(
      "INFO",
      "running isolation_solver",
      {{"batch_id", request->batch_id()},
       {"reference_prices", std::to_string(reference_prices.size())}});

  response->CopyFrom(solver_->Solve(orders, reference_prices));
  response->set_batch_id(request->batch_id());

  return grpc::Status::OK;
}

}  // namespace cex::matching::transport
