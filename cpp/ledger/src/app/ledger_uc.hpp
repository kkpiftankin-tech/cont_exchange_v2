#pragma once
#include <mutex>
#include <string>
#include <unordered_map>

#include "cex/common/decimal.hpp"
#include "fob/ledger/v1/ledger.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "fob/execution/v1/execution.pb.h"

namespace cex::ledger::app {

// In-memory "ledger" for MVP.
// Production: event-sourced + durable storage, but this is enough to demo the dataflow.
class LedgerUseCases {
 public:
  LedgerUseCases();

  fob::ledger::v1::GetBalancesResponse GetBalances(const fob::ledger::v1::GetBalancesRequest& req);
  fob::ledger::v1::ReserveFundsResponse ReserveFunds(const fob::ledger::v1::ReserveFundsRequest& req);
  void ReleaseFunds(const fob::ledger::v1::ReleaseFundsRequest& req);

  fob::ledger::v1::ApplyBatchResultResponse ApplyBatchResult(
      const fob::ledger::v1::ApplyBatchResultRequest& req);

  void ApplyExecutionReport(const fob::ledger::v1::ApplyExecutionReportRequest& req);

 private:
  struct Balance {
    cex::common::Decimal available;
    cex::common::Decimal reserved;
  };

  struct Reservation {
    std::string user_id;
    std::string order_id;
    std::string currency;
    cex::common::Decimal amount;
  };

  using UserBalances = std::unordered_map<std::string, Balance>; // currency -> balance

  mutable std::mutex mu_;
  std::unordered_map<std::string, UserBalances> balances_; // user -> currency -> balance
  std::unordered_map<std::string, Reservation> reservations_; // reservation_id -> reservation

  // Helpers
  Balance& ensure_balance_locked(const std::string& user, const std::string& currency);
};

}  // namespace cex::ledger::app
