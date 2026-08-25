#pragma once
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fob/ledger/v1/ledger.grpc.pb.h"

namespace cex::gateway::infra {

// Thin gRPC client used by the HTTP Gateway to talk to the Ledger service.
// Maps HTTP JSON -> proto -> gRPC call and back. F-06 positions/PnL/margin.
class LedgerClient {
 public:
  explicit LedgerClient(const std::string& target);

  // F-06: user open positions (symbol, side, qty, avg entry, mark, PnL).
  // Returns grpc::Status so the caller can degrade/fail (503) on transport error.
  grpc::Status GetPositions(const fob::ledger::v1::GetPositionsRequest& req,
                            fob::ledger::v1::GetPositionsResponse* resp);

  // Current balances (sync). Optional for richer collateral reporting.
  grpc::Status GetBalances(const fob::ledger::v1::GetBalancesRequest& req,
                           fob::ledger::v1::GetBalancesResponse* resp);

 private:
  std::unique_ptr<fob::ledger::v1::LedgerService::Stub> stub_;
};

}  // namespace cex::gateway::infra
