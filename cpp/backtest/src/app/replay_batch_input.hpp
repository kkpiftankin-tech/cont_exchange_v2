#include <memory>
#include <vector>

#include "fob/matching/v1/solver.grpc.pb.h"
#include "fob/orders/v1/orders.pb.h"
#include "fob/risk/v1/risk.grpc.pb.h"

namespace cex::backtets::app {

class BatchInputReplayer {
 public:
  BatchInputReplayer(
      std::vector<fob::orders::v1::FlowOrder> history,
      std::shared_ptr<fob::matching::v1::Solver::Stub> solver_stub,
      std::shared_ptr<fob::risk::v1::RiskService::Stub> risk_stub)
      : history_(std::move(history)),
        solver_stub_(std::move(solver_stub)),
        risk_stub_(risk_stub) {}

  bool NextTick(const std::vector<fob::orders::v1::FlowOrder>& orders);

 private:
  std::size_t cur_ind_{0};

  std::vector<fob::orders::v1::FlowOrder> history_;
  std::shared_ptr<fob::matching::v1::Solver::Stub> solver_stub_;
  std::shared_ptr<fob::risk::v1::RiskService::Stub> risk_stub_;
};

};  // namespace cex::backtets::app
