#pragma once
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fob/ledger/v1/ledger.grpc.pb.h"

namespace cex::order_flow::infra {

class LedgerClient {
 public:
  explicit LedgerClient(const std::string& target);

  fob::ledger::v1::ReserveFundsResponse ReserveFunds(
      const fob::ledger::v1::ReserveFundsRequest& req);

  void ReleaseFunds(const fob::ledger::v1::ReleaseFundsRequest& req);

  fob::ledger::v1::GetBalancesResponse GetBalances(
      const fob::ledger::v1::GetBalancesRequest& req);

 private:
  std::unique_ptr<fob::ledger::v1::LedgerService::Stub> stub_;
};

}  // namespace cex::order_flow::infra
