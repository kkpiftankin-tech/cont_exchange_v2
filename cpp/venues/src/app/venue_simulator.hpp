#pragma once

#include <cstdint>
#include <string>

#include "cex/common/decimal.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "fob/sim/v1/sim.pb.h"
#include "fob/venue/v1/venue.pb.h"

namespace cex::venues::app {

// F-20 VenueSimulator core (T-F20-301..307).
//
// PURE simulation engine: given a live VenueSnapshot + an order + the
// behaviour models, computes the fill result. It does NOT touch Kafka,
// threads, or SimSession state — those are Phase 4 (VenueSimRouter /
// SimSession Manager) concerns. The LatencyModel here only *samples* a
// latency value (returned in the result); the async "wait before publish"
// is a runtime concern of the caller.
//
// Math is done in double, not Decimal: this is a SIMULATOR feeding the
// isolated, non-authoritative sim-book (ADR-016; spec §1.4 — sim reports
// are not legally-significant execution). CLAUDE.md §9 permits double for
// simulation calculations whose result does not enter the real ledger.
// Final quantities are converted back to Decimal for the report.

struct SimModels {
  fob::sim::v1::LatencyModel latency;
  fob::sim::v1::ImpactModel impact;
  fob::sim::v1::FeeModel fee;
  fob::sim::v1::RejectionModel rejection;
};

struct SimulateRequest {
  std::string venue_id;
  std::string symbol;
  fob::common::v1::Side side{fob::common::v1::SIDE_BUY};
  fob::execution::v1::ExecutionStrategy order_type{
      fob::execution::v1::EXEC_STRATEGY_MARKET};
  cex::common::Decimal target_qty{cex::common::Decimal::zero()};
  bool has_limit_price{false};
  cex::common::Decimal limit_price{cex::common::Decimal::zero()};

  // Live LOB snapshot (from F-11 venue.snapshots). Parallel
  // bid_prices/bid_quantities and ask_prices/ask_quantities.
  const fob::venue::v1::VenueSnapshot* snapshot{nullptr};
  uint32_t lob_age_ms{0};
  uint32_t stale_threshold_ms{2000};
  fob::sim::v1::PartialFillMode partial_fill_mode{
      fob::sim::v1::PARTIAL_FILL_MODE_LEVEL_BY_LEVEL};

  // Deterministic RNG seed for latency / random-reject sampling. Same
  // seed -> same result (replay/test friendly).
  uint64_t rng_seed{0};
};

struct SimulateResult {
  fob::execution::v1::ExecutionReportStatus status{
      fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED};
  cex::common::Decimal filled_qty{cex::common::Decimal::zero()};
  cex::common::Decimal remaining_qty{cex::common::Decimal::zero()};
  cex::common::Decimal avg_price{cex::common::Decimal::zero()};
  cex::common::Decimal fee{cex::common::Decimal::zero()};
  std::string fee_currency;

  // Telemetry -> SimExecutionAnnotation sidecar (ADR-015).
  double impact_bps{0.0};
  int32_t slippage_bps{0};
  uint32_t latency_sample_ms{0};
  std::string lob_snapshot_id;

  std::string reject_reason;   // SIM_STALE_LOB / SIM_NO_LIQUIDITY / SIM_RANDOM_REJECT / SIM_RATE_LIMIT / SIM_PRICE_CONSTRAINT / SIM_TIMEOUT
};

class VenueSimulator {
 public:
  // Pure: no member state mutated. Deterministic given request.rng_seed.
  SimulateResult Simulate(const SimulateRequest& request,
                          const SimModels& models) const;
};

}  // namespace cex::venues::app
