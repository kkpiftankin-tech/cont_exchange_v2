#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "domain/venue_adapter.hpp"
#include "fob/execution/v1/execution.pb.h"

namespace cex::venues::infra {
class VenueSimAdapter;
}  // namespace cex::venues::infra

namespace cex::venues::app {

// F12-BACKTEST-4 — synthetic edge-case scenarios for the VenueSim backtest.
//
// Builds a deterministic library of edge cases that downstream services
// (Risk Manager, Settlement Ledger, ClickHouse) must survive without
// changing business logic:
//   1. Partial fill
//   2. Rejection fallback
//   3. Overfill race
//   4. Venue timeout
//   5. DEX gas spike
//   6. Circuit breaker open
//
// Each scenario is a self-contained recipe: snapshot to inject into
// VenueSim, an ExecutionIntent set, a configurator that wires the right
// VenueSim::OrderPolicy per intent, and the expected ExecutionReport
// shape. The harness (and unit tests) execute the intents through
// ExecuteOnVenue + VenueSimAdapter and assert the outcome via Verify().
class BacktestSyntheticScenarios {
 public:
  enum class ScenarioId {
    kPartialFill = 1,
    kRejectionFallback,
    kOverfillRace,
    kVenueTimeout,
    kDexGasSpike,
    kCircuitBreakerOpen,
  };

  static const char* ToString(ScenarioId id);

  struct ExpectedOutcome {
    // 1-based index aligned with the intent list.
    std::size_t intent_index{0};
    std::string intent_id;
    fob::execution::v1::ExecutionReportStatus expected_status{
        fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED};
    // Inclusive bounds in target_qty units. When max < min the bound is
    // treated as "not checked".
    int64_t min_filled_units{0};
    int64_t max_filled_units{-1};
    // Optional expected error code (substring match on ExecutionReport
    // .error.code). Empty means do not check.
    std::string expected_error_code;
  };

  using AdapterConfigurator =
      std::function<void(infra::VenueSimAdapter& adapter)>;

  struct Scenario {
    ScenarioId id{ScenarioId::kPartialFill};
    std::string name;
    std::string description;
    // Snapshot used by VenueSim's walk_book path. std::nullopt means the
    // scenario does not rely on a book walk (e.g. DEX gas spike).
    std::optional<domain::VenueRawSnapshot> snapshot;
    domain::VenueType venue_type{domain::VenueType::kCex};
    std::vector<fob::execution::v1::ExecutionIntent> intents;
    AdapterConfigurator configure;
    std::vector<ExpectedOutcome> expected;

    // Wires snapshot + policies onto a fresh VenueSimAdapter instance.
    void ApplyTo(infra::VenueSimAdapter& adapter) const;
  };

  struct OutcomeIssue {
    ScenarioId scenario_id{ScenarioId::kPartialFill};
    std::size_t intent_index{0};
    std::string intent_id;
    std::string field;
    std::string expected;
    std::string actual;
  };

  // Builds the full library of edge-case scenarios.
  static std::vector<Scenario> All();

  // Individual builders (exposed for unit tests).
  static Scenario PartialFill();
  static Scenario RejectionFallback();
  static Scenario OverfillRace();
  static Scenario VenueTimeout();
  static Scenario DexGasSpike();
  static Scenario CircuitBreakerOpen();

  // Compares actual reports against the scenario expectations. The
  // returned vector is empty when every expectation holds. The report
  // count is checked first; mismatches add a single issue with
  // field="report_count" and no per-intent diagnostics.
  static std::vector<OutcomeIssue> Verify(
      const Scenario& scenario,
      const std::vector<fob::execution::v1::ExecutionReport>& reports);
};

}  // namespace cex::venues::app
