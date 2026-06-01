#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

#include "cex/common/decimal.hpp"
#include "fob/execution/v1/execution.pb.h"
#include "fob/sim/v1/sim.pb.h"

namespace cex::ledger::app {

// F-20 DoD-7 — input for the sim-book applier. The ExecutionReport uses the
// same binary contract as LIVE (ADR-015); the SimExecutionAnnotation sidecar
// supplies the sim_session_id that the isolated sim_positions table's PK
// requires (ADR-016). Pairing the two by report_id is the consumer's job.
struct SimReportPair {
  fob::execution::v1::ExecutionReport report;
  fob::sim::v1::SimExecutionAnnotation annotation;
};

// Delta to apply to a `sim_positions` row (PK: sim_session_id, provider_id,
// instrument_symbol). signed_qty_delta is positive on BUY and negative on
// SELL. avg_entry / realised_pnl reconciliation is done in the SQL upsert
// (ON CONFLICT DO UPDATE arithmetic) — the applier only emits the trade
// inputs needed by that math.
struct SimPositionDelta {
  std::string sim_session_id;
  std::string provider_id;
  std::string instrument_symbol;
  cex::common::Decimal signed_qty_delta{cex::common::Decimal::zero()};
  cex::common::Decimal exec_price{cex::common::Decimal::zero()};  // > 0
};

// Delta to apply to a `sim_hedge_pnl` row (PK: sim_session_id, venue_id,
// instrument_symbol). All fields are signed; the repo SETs new = old + delta.
struct SimHedgePnlDelta {
  std::string sim_session_id;
  std::string venue_id;
  std::string instrument_symbol;
  cex::common::Decimal hedge_pnl_delta{cex::common::Decimal::zero()};
  cex::common::Decimal fee_delta{cex::common::Decimal::zero()};
  cex::common::Decimal filled_qty_delta{cex::common::Decimal::zero()};
  int64_t trade_count_delta{0};
};

struct SimBookApplyResult {
  bool applied{false};
  std::string skip_reason;  // "duplicate" | "no_session_id" | "non_terminal"
                            // | "zero_fill" | "no_provider" | ""
  std::optional<SimPositionDelta> position_delta;
  std::optional<SimHedgePnlDelta> hedge_pnl_delta;
};

// F-20 DoD-7 — pure sim-book applier. Maps one (sim ExecutionReport,
// SimExecutionAnnotation) pair into deltas for sim_positions and
// sim_hedge_pnl. PURE: no Kafka, no PG, deterministic. Holds an in-memory
// seen-report-ids set so re-deliveries within a process are no-ops; the
// repo's ON CONFLICT DO UPDATE is the across-process safety net.
//
// Gating rules:
//   - report.report_id() empty                       -> skip "duplicate"
//   - annotation.sim_session_id() empty              -> skip "no_session_id"
//   - report.status not FILLED/PARTIALLY_FILLED      -> skip "non_terminal"
//   - report.filled_qty == 0                          -> skip "zero_fill"
//   - report.provider_id() empty                     -> skip "no_provider"
//   - report.report_id() already seen                -> skip "duplicate"
//
// Hedge PnL math (mirrors live ledger's calculate_hedge_pnl, ADR-016):
//   pnl = (avg_price - reference_mid) * filled_qty,
//   sign-flipped for BUY (profit when execution price < reference).
// If reference_mid is absent/zero, hedge_pnl_delta is zero (trade still
// recorded for fee + qty + count totals).
class SimBookApplier {
 public:
  SimBookApplyResult Apply(const SimReportPair& input);

  std::size_t seen_count() const { return seen_report_ids_.size(); }

 private:
  std::unordered_set<std::string> seen_report_ids_;
};

}  // namespace cex::ledger::app
