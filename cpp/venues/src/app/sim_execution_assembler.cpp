#include "app/sim_execution_assembler.hpp"

#include <string>

#include "cex/common/decimal.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

namespace cex::venues::app {

namespace {

using cex::common::Decimal;
using fob::common::v1::Instrument;
using fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
using fob::execution::v1::ExecutionIntent;

std::string CanonicalSymbol(const Instrument& i) {
  if (!i.symbol().empty()) return i.symbol();
  if (!i.base().empty() && !i.quote().empty()) return i.base() + "/" + i.quote();
  return "";
}

bool HasLimitPrice(const ExecutionIntent& intent) {
  return intent.has_limit_price() && intent.limit_price().units() != 0;
}

}  // namespace

SimExecutionOutput SimExecutionAssembler::Assemble(
    const SimExecutionInputs& inputs, const RouteDecision& decision) const {
  const ExecutionIntent& intent = inputs.intent;

  SimulateRequest req;
  req.venue_id = intent.venue();
  req.symbol = CanonicalSymbol(intent.instrument());
  req.side = intent.side();
  req.order_type = intent.strategy();
  req.target_qty = Decimal::from_proto(intent.target_qty());
  req.has_limit_price = HasLimitPrice(intent);
  if (req.has_limit_price) {
    req.limit_price = Decimal::from_proto(intent.limit_price());
  }
  req.snapshot = inputs.snapshot;
  req.lob_age_ms = inputs.lob_age_ms;
  req.stale_threshold_ms = decision.stale_lob_threshold_ms;
  req.partial_fill_mode = decision.partial_fill_mode;
  req.rng_seed = inputs.rng_seed;

  const SimulateResult result = simulator_.Simulate(req, decision.models);

  SimExecutionOutput out;

  // --- ExecutionReport (same contract as LIVE; -> sim.execution.venue) ---
  auto& rep = out.report;
  const std::string report_id = cex::common::uuid_v4();
  const std::string correlation_id = intent.meta().correlation_id().empty()
                                         ? intent.intent_id()
                                         : intent.meta().correlation_id();

  auto* meta = rep.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("venue-simulator");  // distinguishes sim provenance from "venues"
  meta->set_correlation_id(correlation_id);
  meta->set_partition_key(intent.intent_id());

  rep.set_report_id(report_id);
  rep.set_intent_id(intent.intent_id());
  rep.set_hedge_flow_id(intent.hedge_flow_id());
  rep.set_child_order_id(intent.client_order_id());
  rep.set_batch_id(intent.batch_id());
  rep.set_provider_id(intent.provider_id());
  rep.set_venue(intent.venue());
  *rep.mutable_instrument() = intent.instrument();
  rep.set_venue_symbol(intent.venue_symbol());
  rep.set_side(intent.side());
  rep.set_client_order_id(intent.client_order_id());
  rep.set_status(result.status);
  *rep.mutable_filled_qty() = result.filled_qty.to_proto();
  *rep.mutable_remaining_qty() = result.remaining_qty.to_proto();
  *rep.mutable_average_price() = result.avg_price.to_proto();
  rep.set_slippage_bps(result.slippage_bps);
  if (intent.has_reference_mid()) {
    *rep.mutable_reference_mid() = intent.reference_mid();
  }

  if (result.fee.units != 0 || !result.fee_currency.empty()) {
    auto* fee = rep.mutable_fee_total();
    fee->set_fee_type("taker");
    auto* cost = fee->mutable_cost();
    cost->set_currency(result.fee_currency);
    *cost->mutable_amount() = result.fee.to_proto();
  }

  if (result.status == EXECUTION_REPORT_STATUS_REJECTED &&
      !result.reject_reason.empty()) {
    auto* err = rep.mutable_error();
    err->set_code(result.reject_reason);
    err->set_message("sim reject: " + result.reject_reason);
  }

  // --- SimExecutionAnnotation (sidecar; -> sim.execution.annotations) ---
  auto& ann = out.annotation;
  auto* ameta = ann.mutable_meta();
  ameta->set_event_id(cex::common::uuid_v4());
  *ameta->mutable_ts_event() = cex::common::now_ts();
  ameta->set_source("venue-simulator");
  ameta->set_correlation_id(correlation_id);
  ameta->set_partition_key(intent.intent_id());

  ann.set_report_id(report_id);  // SAME id -> correlates to the report
  ann.set_sim_session_id(decision.sim_session_id);
  ann.set_lob_snapshot_id(result.lob_snapshot_id);
  ann.set_lob_age_ms(inputs.lob_age_ms);
  ann.set_impact_bps(result.impact_bps);
  ann.set_latency_sample_ms(result.latency_sample_ms);

  return out;
}

}  // namespace cex::venues::app
