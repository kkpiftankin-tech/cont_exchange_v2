#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "app/backtest_synthetic_scenarios.hpp"
#include "app/execute_on_venue.hpp"
#include "domain/venue_adapter.hpp"
#include "fob/execution/v1/execution.pb.h"
#include "infra/venue_sim_adapter.hpp"

namespace {

using cex::venues::app::BacktestSyntheticScenarios;
using cex::venues::app::ExecuteOnVenue;
using cex::venues::domain::VenueType;
using cex::venues::infra::VenueSimAdapter;

bool Check(const bool ok, const std::string& msg) {
  if (ok) return true;
  std::cerr << "[FAIL] " << msg << std::endl;
  return false;
}

std::vector<fob::execution::v1::ExecutionReport> RunScenario(
    const BacktestSyntheticScenarios::Scenario& scenario) {
  VenueSimAdapter adapter(
      scenario.intents.empty() ? "binance"
                               : scenario.intents.front().venue(),
      scenario.venue_type);
  scenario.ApplyTo(adapter);

  ExecuteOnVenue executor;
  std::vector<fob::execution::v1::ExecutionReport> out;
  out.reserve(scenario.intents.size());
  for (const auto& intent : scenario.intents) {
    out.push_back(executor.Run(intent, &adapter));
  }
  return out;
}

bool TestAllScenariosBuilt() {
  const auto all = BacktestSyntheticScenarios::All();
  if (!Check(all.size() == 6, "All() must return six synthetic scenarios"))
    return false;
  const std::vector<BacktestSyntheticScenarios::ScenarioId> expected_ids = {
      BacktestSyntheticScenarios::ScenarioId::kPartialFill,
      BacktestSyntheticScenarios::ScenarioId::kRejectionFallback,
      BacktestSyntheticScenarios::ScenarioId::kOverfillRace,
      BacktestSyntheticScenarios::ScenarioId::kVenueTimeout,
      BacktestSyntheticScenarios::ScenarioId::kDexGasSpike,
      BacktestSyntheticScenarios::ScenarioId::kCircuitBreakerOpen,
  };
  for (std::size_t i = 0; i < expected_ids.size(); ++i) {
    if (!Check(all[i].id == expected_ids[i],
               std::string("Scenario order differs at index ") +
                   std::to_string(i)))
      return false;
    if (!Check(!all[i].name.empty(), "Scenario name must be non-empty"))
      return false;
    if (!Check(!all[i].intents.empty(),
               "Scenario must define at least one intent"))
      return false;
    if (!Check(all[i].intents.size() == all[i].expected.size(),
               "Scenario expectations must match the intent list size"))
      return false;
  }
  return true;
}

bool TestPartialFillRunsAndVerifies() {
  const auto scenario = BacktestSyntheticScenarios::PartialFill();
  const auto reports = RunScenario(scenario);
  const auto issues = BacktestSyntheticScenarios::Verify(scenario, reports);

  if (!Check(issues.empty(), "PartialFill scenario must verify clean"))
    return false;
  if (!Check(reports.size() == 1,
             "PartialFill scenario must produce exactly one report"))
    return false;
  if (!Check(reports.front().status() ==
                 fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
             "PartialFill must report PARTIALLY_FILLED"))
    return false;
  if (!Check(reports.front().filled_qty().units() == 100,
             "PartialFill must fill the available ask depth (100)"))
    return false;
  if (!Check(reports.front().remaining_qty().units() == 150,
             "PartialFill remaining_qty = target - filled"))
    return false;
  return true;
}

bool TestRejectionFallback() {
  const auto scenario = BacktestSyntheticScenarios::RejectionFallback();
  const auto reports = RunScenario(scenario);
  const auto issues = BacktestSyntheticScenarios::Verify(scenario, reports);

  if (!Check(issues.empty(),
             "RejectionFallback expectations must verify clean"))
    return false;
  if (!Check(reports.size() == 2,
             "RejectionFallback scenario must emit two reports"))
    return false;
  if (!Check(reports[0].status() ==
                 fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "Primary intent must be REJECTED by VenueSim"))
    return false;
  if (!Check(reports[0].error().code() == "INVALID_PRICE",
             "Primary reject must carry the INVALID_PRICE error code"))
    return false;
  if (!Check(reports[1].status() ==
                 fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "Fallback intent must fully fill"))
    return false;
  if (!Check(reports[1].filled_qty().units() == 200,
             "Fallback intent must fill its target qty"))
    return false;
  return true;
}

bool TestOverfillRaceFillsBoth() {
  // VenueSim alone cannot reproduce the overfill guard (that lives in
  // Venue Execution Adapter). The synthetic scenario reproduces the
  // race condition where two child orders BOTH fully fill — the
  // downstream guard then has the responsibility to detect the
  // aggregate breach.
  const auto scenario = BacktestSyntheticScenarios::OverfillRace();
  const auto reports = RunScenario(scenario);
  const auto issues = BacktestSyntheticScenarios::Verify(scenario, reports);

  if (!Check(issues.empty(),
             "OverfillRace expectations must verify clean"))
    return false;
  if (!Check(reports.size() == 2,
             "OverfillRace scenario must emit two reports"))
    return false;
  const int64_t aggregated =
      reports[0].filled_qty().units() + reports[1].filled_qty().units();
  if (!Check(aggregated == 550,
             "Aggregated fill across the race must equal sum of intents"))
    return false;
  for (const auto& r : reports) {
    if (!Check(r.status() ==
                   fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
               "Both racing child orders must report FILLED"))
      return false;
  }
  return true;
}

bool TestVenueTimeoutSurfacesCancelled() {
  const auto scenario = BacktestSyntheticScenarios::VenueTimeout();
  const auto reports = RunScenario(scenario);
  const auto issues = BacktestSyntheticScenarios::Verify(scenario, reports);

  if (!Check(issues.empty(),
             "VenueTimeout expectations must verify clean"))
    return false;
  if (!Check(reports.size() == 1,
             "VenueTimeout produces a single report"))
    return false;
  if (!Check(reports[0].status() ==
                 fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED,
             "VenueTimeout intent must end CANCELLED"))
    return false;
  if (!Check(reports[0].filled_qty().units() == 0,
             "VenueTimeout intent must not be filled"))
    return false;
  return true;
}

bool TestDexGasSpikeRejected() {
  const auto scenario = BacktestSyntheticScenarios::DexGasSpike();
  const auto reports = RunScenario(scenario);
  const auto issues = BacktestSyntheticScenarios::Verify(scenario, reports);

  if (!Check(issues.empty(),
             "DexGasSpike expectations must verify clean"))
    return false;
  if (!Check(reports.size() == 1,
             "DexGasSpike scenario must emit a single report"))
    return false;
  if (!Check(reports[0].status() ==
                 fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "DexGasSpike intent must be REJECTED"))
    return false;
  if (!Check(reports[0].error().code() == "TX_FAILED",
             "DexGasSpike must propagate TX_FAILED error code"))
    return false;
  return true;
}

bool TestCircuitBreakerOpenRejectsAll() {
  const auto scenario = BacktestSyntheticScenarios::CircuitBreakerOpen();
  const auto reports = RunScenario(scenario);
  const auto issues = BacktestSyntheticScenarios::Verify(scenario, reports);

  if (!Check(issues.empty(),
             "CircuitBreakerOpen expectations must verify clean"))
    return false;
  if (!Check(reports.size() == 2,
             "CircuitBreakerOpen scenario covers BUY + SELL"))
    return false;
  for (const auto& r : reports) {
    if (!Check(r.status() ==
                   fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
               "Every intent must be REJECTED while breaker is OPEN"))
      return false;
    if (!Check(r.error().code() == "VENUE_UNAVAILABLE",
               "Reject reason must be VENUE_UNAVAILABLE"))
      return false;
    if (!Check(r.filled_qty().units() == 0,
               "No fill expected when circuit breaker is OPEN"))
      return false;
  }
  return true;
}

bool TestVerifyDetectsStatusMismatch() {
  auto scenario = BacktestSyntheticScenarios::PartialFill();
  // Force the expectation to a wrong status — Verify must flag it.
  scenario.expected.front().expected_status =
      fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
  const auto reports = RunScenario(scenario);
  const auto issues = BacktestSyntheticScenarios::Verify(scenario, reports);

  if (!Check(!issues.empty(),
             "Verify must surface at least one issue on a forced mismatch"))
    return false;
  bool status_issue = false;
  for (const auto& iss : issues) {
    if (iss.field == "status") status_issue = true;
  }
  if (!Check(status_issue, "status mismatch must be reported"))
    return false;
  return true;
}

bool TestVerifyDetectsFilledQtyOutOfRange() {
  auto scenario = BacktestSyntheticScenarios::PartialFill();
  // PartialFill scenario fills exactly 100 units. Demand 150..200 to
  // force a range violation.
  scenario.expected.front().min_filled_units = 150;
  scenario.expected.front().max_filled_units = 200;
  const auto reports = RunScenario(scenario);
  const auto issues = BacktestSyntheticScenarios::Verify(scenario, reports);

  bool range_issue = false;
  for (const auto& iss : issues) {
    if (iss.field == "filled_qty.units") range_issue = true;
  }
  if (!Check(range_issue,
             "filled_qty.units out-of-range must be reported"))
    return false;
  return true;
}

bool TestVerifyDetectsReportCountMismatch() {
  const auto scenario = BacktestSyntheticScenarios::CircuitBreakerOpen();
  // Pass only one report when the scenario expects two.
  std::vector<fob::execution::v1::ExecutionReport> truncated;
  truncated.push_back(RunScenario(scenario).front());
  const auto issues =
      BacktestSyntheticScenarios::Verify(scenario, truncated);

  if (!Check(issues.size() == 1,
             "Report count mismatch must short-circuit to a single issue"))
    return false;
  if (!Check(issues.front().field == "report_count",
             "Mismatch field must be report_count"))
    return false;
  return true;
}

bool TestScenarioIdToString() {
  if (!Check(std::string(BacktestSyntheticScenarios::ToString(
                 BacktestSyntheticScenarios::ScenarioId::kPartialFill)) ==
                 "partial_fill",
             "ScenarioId::kPartialFill must stringify to partial_fill"))
    return false;
  if (!Check(std::string(BacktestSyntheticScenarios::ToString(
                 BacktestSyntheticScenarios::ScenarioId::
                     kCircuitBreakerOpen)) == "circuit_breaker_open",
             "ScenarioId::kCircuitBreakerOpen stringify"))
    return false;
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestAllScenariosBuilt() && ok;
  ok = TestPartialFillRunsAndVerifies() && ok;
  ok = TestRejectionFallback() && ok;
  ok = TestOverfillRaceFillsBoth() && ok;
  ok = TestVenueTimeoutSurfacesCancelled() && ok;
  ok = TestDexGasSpikeRejected() && ok;
  ok = TestCircuitBreakerOpenRejectsAll() && ok;
  ok = TestVerifyDetectsStatusMismatch() && ok;
  ok = TestVerifyDetectsFilledQtyOutOfRange() && ok;
  ok = TestVerifyDetectsReportCountMismatch() && ok;
  ok = TestScenarioIdToString() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] backtest_synthetic_scenarios_test" << std::endl;
  return EXIT_SUCCESS;
}
