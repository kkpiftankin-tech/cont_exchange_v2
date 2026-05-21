#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/execute_on_venue.hpp"
#include "domain/venue_adapter.hpp"
#include "fob/execution/v1/execution.pb.h"

namespace cex::venues::app {

// F12-BACKTEST-3 — backtest parity check.
//
// Runs the same ExecutionIntent set against multiple independent VenueSim
// adapter instances and verifies that the resulting ExecutionReport set is
// deterministic on the fields downstream services rely on:
//   - status
//   - filled_qty (units, scale)
//   - average_price (units, scale)
//   - slippage_bps
//   - fee_total.cost.amount + fee_currency
//   - error_code
// plus the aggregate reconciliation_gap (sum of target_qty.units -
// filled_qty.units, in qty-scale of the first intent).
class BacktestParityCheck {
 public:
  // Free-form per-intent / per-adapter setup hook. Invoked once per run
  // before the intents are executed so the caller can inject snapshots,
  // wire OrderPolicy, etc. The run index lets the caller distinguish runs
  // for diagnostics, but the hook must be deterministic across runs for
  // parity to hold.
  using AdapterConfigurator =
      std::function<void(domain::VenueAdapter& adapter, std::size_t run_index)>;
  using AdapterFactory =
      std::function<std::unique_ptr<domain::VenueAdapter>(std::size_t run_index)>;

  struct FieldMismatch {
    std::size_t intent_index{0};
    std::string intent_id;
    std::string field;
    std::string baseline;
    std::string candidate;
  };

  struct RunStats {
    std::size_t intents_executed{0};
    int64_t total_target_qty_units{0};
    int64_t total_filled_qty_units{0};
    int64_t reconciliation_gap_units{0};
    int32_t qty_scale{0};
  };

  struct ParityReport {
    bool deterministic{true};
    std::size_t runs{0};
    std::size_t intents{0};
    std::vector<RunStats> per_run;
    // Mismatches detected when comparing the second (and later) runs
    // against the first one.
    std::vector<FieldMismatch> mismatches;
  };

  BacktestParityCheck() = default;

  // Runs `intents` `runs` times. `factory` must return a fresh adapter for
  // each run (the parity check owns the lifecycle). `configure` is called
  // after the adapter is constructed but before SendOrder so backtest
  // scenarios (snapshot injection, per-intent policy) can be wired
  // identically per run.
  ParityReport Run(const std::vector<fob::execution::v1::ExecutionIntent>& intents,
                   const AdapterFactory& factory,
                   const AdapterConfigurator& configure,
                   std::size_t runs = 2);
};

}  // namespace cex::venues::app
