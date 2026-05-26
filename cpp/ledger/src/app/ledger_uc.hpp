#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cex/common/decimal.hpp"
#include "app/persistence_ports.hpp"
#include "fob/ledger/v1/ledger.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "fob/execution/v1/execution.pb.h"

namespace cex::ledger::app {

// In-memory "ledger" for MVP.
// Production: event-sourced + durable storage, but this is enough to demo the dataflow.
class LedgerUseCases {
 public:
  struct InitOptions {
    bool seed_demo_state{true};
  };

  // Position for PnL calculation
  struct Position {
    cex::common::Decimal amount{0, 0};           // net position (positive = long)
    cex::common::Decimal avg_entry_price{0, 0};  // average entry price
    cex::common::Decimal realised_pnl{0, 0};     // cumulative realised PnL
  };

  // Venue balance structure
  struct VenueBalanceEntry {
    cex::common::Decimal total{0, 0};      // total balance on venue
    cex::common::Decimal reserved{0, 0};   // reserved for open orders
    cex::common::Decimal available{0, 0};  // free balance (total - reserved)
    std::chrono::system_clock::time_point updated_at;
  };

  // Hedge PnL record structure
  struct HedgePnlRecord {
    std::string hedge_id;
    std::string venue;
    std::string instrument_symbol;
    fob::common::v1::Side side;
    cex::common::Decimal executed_qty;
    cex::common::Decimal executed_price;
    cex::common::Decimal internal_price;
    cex::common::Decimal hedge_pnl;
    std::chrono::system_clock::time_point timestamp;
  };

  LedgerUseCases();
  explicit LedgerUseCases(InitOptions options);
  LedgerUseCases(std::shared_ptr<PositionsRepositoryPort> positions_repo,
                 std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo);
  LedgerUseCases(InitOptions options,
                 std::shared_ptr<PositionsRepositoryPort> positions_repo,
                 std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo);
  LedgerUseCases(InitOptions options,
                 std::shared_ptr<PositionsRepositoryPort> positions_repo,
                 std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo,
                 std::shared_ptr<HedgeLedgerEntriesRepositoryPort> hedge_entries_repo);

  // Existing methods
  fob::ledger::v1::GetBalancesResponse GetBalances(const fob::ledger::v1::GetBalancesRequest& req);
  fob::ledger::v1::ReserveFundsResponse ReserveFunds(const fob::ledger::v1::ReserveFundsRequest& req);
  void ReleaseFunds(const fob::ledger::v1::ReleaseFundsRequest& req);
  fob::ledger::v1::ApplyBatchResultResponse ApplyBatchResult(
      const fob::ledger::v1::ApplyBatchResultRequest& req);
  void RememberExecutionIntent(const fob::execution::v1::ExecutionIntent& intent);
  void ApplyExecutionReport(const fob::ledger::v1::ApplyExecutionReportRequest& req);
  void SetIdempotencyRepo(std::shared_ptr<IdempotencyRepositoryPort> repo) {
    idempotency_repo_ = std::move(repo);
  }
  void SetHedgeEntriesRepo(std::shared_ptr<HedgeLedgerEntriesRepositoryPort> repo) {
    hedge_entries_repo_ = std::move(repo);
  }
  // F-12 / IN-009 DoD-6 (PR-F12-3c): sink that accumulates hedge_pnl/fee
  // deltas into PG `hedgeflows` row. Optional — nullptr is valid.
  void SetHedgeflowPnlSink(std::shared_ptr<HedgeflowPnlSinkPort> sink) {
    hedgeflow_pnl_sink_ = std::move(sink);
  }
  void SeedBalance(const std::string& user_id,
                   const std::string& currency,
                   const cex::common::Decimal& available,
                   const cex::common::Decimal& reserved = cex::common::Decimal::zero());
  void SeedPosition(const std::string& user_id,
                    const std::string& instrument_symbol,
                    const cex::common::Decimal& amount,
                    const cex::common::Decimal& avg_entry_price,
                    const cex::common::Decimal& realised_pnl =
                        cex::common::Decimal::zero());
  Position GetPosition(const std::string& user_id, const std::string& instrument_symbol) const;
  std::unordered_map<std::string, Position> GetPositions(const std::string& user_id) const;

  struct ExecutionReconciliationStats {
    uint64_t queued_reports{0};
    uint64_t replayed_reports{0};
    uint64_t applied_reports{0};
    uint64_t duplicate_reports{0};
    uint64_t missing_plan_reports{0};
    uint64_t plan_mismatch_reports{0};
    uint64_t status_regression_reports{0};
    uint64_t qty_regression_reports{0};
  };

  ExecutionReconciliationStats GetExecutionReconciliationStats() const;

  fob::ledger::v1::GetUnrealisedPnLResponse GetUnrealisedPnL(
      const fob::ledger::v1::GetUnrealisedPnLRequest& req,
      const std::unordered_map<std::string, cex::common::Decimal>& current_prices);
  fob::ledger::v1::GetRealisedPnLResponse GetRealisedPnL(
      const fob::ledger::v1::GetRealisedPnLRequest& req);

  fob::ledger::v1::GetVenueBalancesResponse GetVenueBalances(
      const fob::ledger::v1::GetVenueBalancesRequest& req);
  fob::ledger::v1::UpdateVenueBalanceResponse UpdateVenueBalance(
      const fob::ledger::v1::UpdateVenueBalanceRequest& req);

  // Record a hedge execution for PnL tracking
  fob::ledger::v1::RecordHedgeExecutionResponse RecordHedgeExecution(
      const fob::ledger::v1::RecordHedgeExecutionRequest& req);

  // Get hedge PnL summary
  fob::ledger::v1::GetHedgePnLResponse GetHedgePnL(
      const fob::ledger::v1::GetHedgePnLRequest& req);

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

  struct ExecutionLedgerState {
    fob::execution::v1::ExecutionReportStatus status{fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED};
    cex::common::Decimal filled_qty{0, 0};
    cex::common::Decimal remaining_qty{0, 0};
    cex::common::Decimal average_price{0, 0};
    cex::common::Decimal fee_total{0, 0};
    std::string fee_currency;
    bool terminal{false};
  };

  using UserBalances = std::unordered_map<std::string, Balance>; // currency -> balance
  using UserPositions = std::unordered_map<std::string, Position>; // instrument -> position
  using VenueBalances = std::unordered_map<std::string, VenueBalanceEntry>; // currency -> balance
  using VenueMap = std::unordered_map<std::string, VenueBalances>; // venue -> currency -> balance
  using HedgePnlMap = std::unordered_map<std::string, std::vector<HedgePnlRecord>>; // venue -> records

  mutable std::mutex mu_;
  std::unordered_map<std::string, UserBalances> balances_; // user -> currency -> balance
  std::unordered_map<std::string, UserPositions> positions_; // user -> instrument -> position
  std::unordered_map<std::string, Reservation> reservations_; // reservation_id -> reservation
  VenueMap venue_balances_; // venue -> currency -> balance
  HedgePnlMap hedge_pnl_records_; // venue -> list of hedge records
  std::unordered_map<std::string, cex::common::Decimal> hedge_pnl_summary_; // venue:currency -> total PnL
  std::unordered_map<std::string, fob::execution::v1::ExecutionIntent> execution_intents_; // intent_id -> plan
  std::unordered_map<std::string, std::vector<fob::execution::v1::ExecutionReport>> pending_execution_reports_;
  std::unordered_map<std::string, ExecutionLedgerState> execution_states_; // intent_id|order_key -> state
  std::unordered_set<std::string> seen_execution_report_keys_;
  ExecutionReconciliationStats exec_recon_stats_;

  std::shared_ptr<PositionsRepositoryPort> positions_repo_;
  std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo_;
  std::shared_ptr<IdempotencyRepositoryPort> idempotency_repo_;
  std::shared_ptr<HedgeLedgerEntriesRepositoryPort> hedge_entries_repo_;
  // F-12 / IN-009 DoD-6 (PR-F12-3c) — optional PG sink for hedge_pnl/fee.
  std::shared_ptr<HedgeflowPnlSinkPort> hedgeflow_pnl_sink_;

  // Helpers
  Balance& ensure_balance_locked(const std::string& user, const std::string& currency);
  Position& ensure_position_locked(const std::string& user, const std::string& instrument);
  VenueBalanceEntry& ensure_venue_balance_locked(const std::string& venue, const std::string& currency);
  
  // PnL calculation helpers
  void update_position_for_fill(const fob::matching::v1::FlowFill& fill);
  void calculate_and_record_pnl(const std::string& user, 
                                 const std::string& instrument,
                                 const cex::common::Decimal& sell_qty,
                                 const cex::common::Decimal& sell_price);
  
  // Hedge PnL helper
  cex::common::Decimal calculate_hedge_pnl(
      fob::common::v1::Side side,
      const cex::common::Decimal& executed_price,
      const cex::common::Decimal& internal_price,
      const cex::common::Decimal& qty);

  bool apply_execution_report_locked(const fob::execution::v1::ExecutionReport& report);
};

}  // namespace cex::ledger::app
