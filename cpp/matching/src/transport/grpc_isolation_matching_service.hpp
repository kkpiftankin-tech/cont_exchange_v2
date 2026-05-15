#pragma once

#include "fob/matching/v1/solver.grpc.pb.h"

#include "domain/solver.hpp"

namespace cex::matching::transport {

class GrpcIsolationSolverService final
    : public fob::matching::v1::Solver::Service {
public:
  explicit GrpcIsolationSolverService(domain::IContinuousClearingSolver *solver)
      : solver_(solver) {}

  grpc::Status Solve(grpc::ServerContext *context,
                     const fob::matching::v1::BatchRequest *request,
                     fob::matching::v1::BatchResult *response) override;

private:
  domain::IContinuousClearingSolver *solver_;
};

} // namespace cex::matching::transport
