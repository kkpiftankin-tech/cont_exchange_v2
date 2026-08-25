#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "app/backtest_parity_check.hpp"
#include "domain/venue_adapter.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "infra/venue_sim_adapter.hpp"

namespace {

using cex::venues::app::BacktestParityCheck;
using cex::venues::domain::VenueAdapter;
using cex::venues::domain::VenueBookLevel;
using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueRawSnapshot;
using cex::venues::domain::VenueType;
using cex::venues::infra::VenueSimAdapter;

bool Check(const bool ok, const std::string& msg) {
  if (ok) return true;
  std::cerr << "[FAIL] " << msg << std::endl;
  return false;
}

fob::common::v1::Instrument BtcUsdt() {
  fob::common::v1::Instrument i;
  i.set_symbol("BTC/USDT");
  i.set_base("BTC");
  i.set_quote("USDT");
  return i;
}

fob::execution::v1::ExecutionIntent BuildBuy(const std::string& id,
                                             const int64_t qty_units,
                                             const int64_t limit_units = 0) {
  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id(id);
  intent.set_client_order_id(id + "-c");
  intent.set_venue("binance");
  intent.set_venue_symbol("BTCUSDT");
  *intent.mutable_instrument() = BtcUsdt();
  intent.set_side(fob::common::v1::SIDE_BUY);
  intent.mutable_target_qty()->set_units(qty_units);
  intent.mutable_target_qty()->set_scale(3);
  if (limit_units != 0) {
    intent.mutable_limit_price()->set_units(limit_units);
    intent.mutable_limit_price()->set_scale(2);
  }
  return intent;
}

VenueRawSnapshot MakeSnapshot(
    const std::vector<std::pair<int64_t, int64_t>>& bids,
    const std::vector<std::pair<int64_t, int64_t>>& asks) {
  VenueRawSnapshot s;
  s.venue_id = "binance";
  s.venue_type = VenueType::kCex;
  s.instrument = BtcUsdt();
  s.venue_symbol = "BTCUSDT";
  s.status = VenueConnectionStatus::kConnected;
  s.mid_price.units = 6800000;
  s.mid_price.scale = 2;
  for (const auto& [p, q] : bids) {
    VenueBookLevel level;
    level.price.units = p; level.price.scale = 2;
    level.qty.units = q;   level.qty.scale = 3;
    s.bids.push_back(level);
  }
  for (const auto& [p, q] : asks) {
    VenueBookLevel level;
    level.price.units = p; level.price.scale = 2;
    level.qty.units = q;   level.qty.scale = 3;
    s.asks.push_back(level);
  }
  if (!s.bids.empty()) s.best_bid = s.bids.front().price;
  if (!s.asks.empty()) s.best_ask = s.asks.front().price;
  return s;
}

auto WalkBookFactory() {
  return [](std::size_t /*run_idx*/) -> std::unique_ptr<VenueAdapter> {
    return std::make_unique<VenueSimAdapter>("binance", VenueType::kCex);
  };
}

BacktestParityCheck::AdapterConfigurator WalkBookConfigure(
    const VenueRawSnapshot& snapshot,
    const int32_t taker_fee_bps = 10,
    const bool enforce_limit = true) {
  return [=](VenueAdapter& adapter, std::size_t /*run_idx*/) {
    auto* sim = dynamic_cast<VenueSimAdapter*>(&adapter);
    if (sim == nullptr) return;
    sim->Connect();
    sim->SetLastSnapshot(snapshot);
    VenueSimAdapter::OrderPolicy policy;
    policy.walk_book = true;
    policy.taker_fee_bps = taker_fee_bps;
    policy.enforce_limit_price = enforce_limit;
    sim->SetDefaultOrderPolicy(policy);
  };
}

bool TestParityIdenticalIntentsDeterministic() {
  const auto snap = MakeSnapshot({}, {{6800000, 200}, {6800500, 300}});
  std::vector<fob::execution::v1::ExecutionIntent> intents = {
      BuildBuy("i-1", 100),
      BuildBuy("i-2", 250),
      BuildBuy("i-3", 50),
  };

  BacktestParityCheck checker;
  const auto report = checker.Run(intents, WalkBookFactory(),
                                  WalkBookConfigure(snap), /*runs=*/3);

  if (!Check(report.deterministic,
             "Three runs against identical state must be deterministic"))
    return false;
  if (!Check(report.mismatches.empty(),
             "No field mismatches expected"))
    return false;
  if (!Check(report.per_run.size() == 3,
             "Per-run stats must record all runs"))
    return false;
  for (const auto& stats : report.per_run) {
    if (!Check(stats.intents_executed == 3,
               "Each run must execute every intent"))
      return false;
  }
  return true;
}

bool TestReconciliationGapAggregation() {
  // VenueSim walks the cached snapshot per call (the book is not consumed
  // between intents within a run). Each intent therefore caps at the
  // configured ask depth of 100, producing predictable per-intent gaps.
  const auto snap = MakeSnapshot({}, {{6800000, 100}});

  std::vector<fob::execution::v1::ExecutionIntent> intents = {
      BuildBuy("a", 50),   // FILLED 50
      BuildBuy("b", 200),  // PARTIAL 100
      BuildBuy("c", 100),  // FILLED 100
  };

  BacktestParityCheck checker;
  const auto report = checker.Run(intents, WalkBookFactory(),
                                  WalkBookConfigure(snap));

  if (!Check(report.deterministic,
             "Parity must hold across runs with same snapshot"))
    return false;

  const auto& stats = report.per_run.front();
  // Target = 50 + 200 + 100 = 350; filled = 50 + 100 + 100 = 250; gap = 100
  if (!Check(stats.total_target_qty_units == 350,
             "Aggregated target qty must equal sum of intents"))
    return false;
  if (!Check(stats.total_filled_qty_units == 250,
             "Aggregated filled qty must reflect per-intent fills"))
    return false;
  if (!Check(stats.reconciliation_gap_units == 100,
             "reconciliation_gap = target - filled"))
    return false;
  return true;
}

bool TestParityDetectsMismatchAcrossRuns() {
  // Use a configurator that flips taker_fee_bps for the second run — this
  // must surface as a mismatch on fee_total.cost.amount.
  const auto snap = MakeSnapshot({}, {{6800000, 1000}});
  std::vector<fob::execution::v1::ExecutionIntent> intents = {
      BuildBuy("m-1", 100),
  };

  BacktestParityCheck checker;
  const auto report = checker.Run(
      intents, WalkBookFactory(),
      [snap](VenueAdapter& adapter, std::size_t run_idx) {
        auto* sim = dynamic_cast<VenueSimAdapter*>(&adapter);
        if (sim == nullptr) return;
        sim->Connect();
        sim->SetLastSnapshot(snap);
        VenueSimAdapter::OrderPolicy policy;
        policy.walk_book = true;
        policy.taker_fee_bps = run_idx == 0 ? 10 : 50;  // non-deterministic
        sim->SetDefaultOrderPolicy(policy);
      },
      /*runs=*/2);

  if (!Check(!report.deterministic,
             "Differing fee policy across runs must break parity"))
    return false;
  bool fee_mismatch_seen = false;
  for (const auto& m : report.mismatches) {
    if (m.field == "fee_total.cost.amount") fee_mismatch_seen = true;
  }
  if (!Check(fee_mismatch_seen,
             "Mismatch on fee_total.cost.amount must be reported"))
    return false;
  return true;
}

bool TestParityDetectsStatusMismatch() {
  // First run: snapshot has full liquidity (FILLED). Second run: empty
  // asks (REJECTED). Must produce status + filled_qty mismatches.
  const auto full_book = MakeSnapshot({}, {{6800000, 1000}});
  const auto empty_book = MakeSnapshot({}, {});

  std::vector<fob::execution::v1::ExecutionIntent> intents = {
      BuildBuy("s-1", 200),
  };

  std::vector<VenueRawSnapshot> per_run = {full_book, empty_book};
  BacktestParityCheck checker;
  const auto report = checker.Run(
      intents, WalkBookFactory(),
      [&per_run](VenueAdapter& adapter, std::size_t run_idx) {
        auto* sim = dynamic_cast<VenueSimAdapter*>(&adapter);
        if (sim == nullptr) return;
        sim->Connect();
        sim->SetLastSnapshot(per_run[run_idx]);
        VenueSimAdapter::OrderPolicy policy;
        policy.walk_book = true;
        sim->SetDefaultOrderPolicy(policy);
      },
      /*runs=*/2);

  if (!Check(!report.deterministic,
             "Different snapshots across runs must break parity"))
    return false;
  bool status_seen = false;
  bool gap_seen = false;
  for (const auto& m : report.mismatches) {
    if (m.field == "status") status_seen = true;
    if (m.field == "reconciliation_gap_units") gap_seen = true;
  }
  if (!Check(status_seen, "Status mismatch must be reported")) return false;
  if (!Check(gap_seen,
             "reconciliation_gap_units mismatch must be reported"))
    return false;
  return true;
}

bool TestEmptyInputsAreDeterministicNoop() {
  BacktestParityCheck checker;
  const auto report = checker.Run({}, WalkBookFactory(),
                                  WalkBookConfigure(MakeSnapshot({}, {})));
  if (!Check(report.deterministic,
             "Empty intent set must be considered deterministic"))
    return false;
  if (!Check(report.intents == 0 && report.per_run.empty(),
             "Empty input produces no per-run stats"))
    return false;
  return true;
}

bool TestExecutionReportSetMatchesAcrossRuns() {
  // Verifies that the per-intent ExecutionReport projections (status,
  // filled_qty, average_price, slippage_bps, fee) match across runs —
  // this is the "ExecutionReport set" parity requirement from
  // F12-BACKTEST-3.
  const auto snap = MakeSnapshot({{6799500, 1000}}, {{6800000, 1000}});

  std::vector<fob::execution::v1::ExecutionIntent> intents;
  for (int i = 0; i < 5; ++i) {
    intents.push_back(BuildBuy("set-" + std::to_string(i), 100 + i * 20));
  }
  // Mix a SELL too.
  auto sell = BuildBuy("sell-1", 80);
  sell.set_side(fob::common::v1::SIDE_SELL);
  intents.push_back(sell);

  BacktestParityCheck checker;
  const auto report = checker.Run(intents, WalkBookFactory(),
                                  WalkBookConfigure(snap), /*runs=*/4);

  if (!Check(report.deterministic,
             "Mixed BUY/SELL set must remain deterministic across 4 runs"))
    return false;
  for (std::size_t r = 1; r < report.per_run.size(); ++r) {
    if (!Check(report.per_run[r].total_filled_qty_units ==
                   report.per_run.front().total_filled_qty_units,
               "filled qty aggregates must be identical across runs"))
      return false;
    if (!Check(report.per_run[r].reconciliation_gap_units ==
                   report.per_run.front().reconciliation_gap_units,
               "reconciliation_gap must be identical across runs"))
      return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestParityIdenticalIntentsDeterministic() && ok;
  ok = TestReconciliationGapAggregation() && ok;
  ok = TestParityDetectsMismatchAcrossRuns() && ok;
  ok = TestParityDetectsStatusMismatch() && ok;
  ok = TestEmptyInputsAreDeterministicNoop() && ok;
  ok = TestExecutionReportSetMatchesAcrossRuns() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] backtest_parity_check_test" << std::endl;
  return EXIT_SUCCESS;
}
