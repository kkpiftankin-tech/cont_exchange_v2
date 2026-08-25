#include "app/backtest_synthetic_scenarios.hpp"

#include <sstream>
#include <utility>

#include "infra/venue_sim_adapter.hpp"

namespace cex::venues::app {

namespace {

using fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
using fob::execution::v1::ExecutionIntent;
using fob::execution::v1::ExecutionReport;
using fob::execution::v1::ExecutionReportStatus;

fob::common::v1::Instrument BtcUsdt() {
  fob::common::v1::Instrument i;
  i.set_symbol("BTC/USDT");
  i.set_base("BTC");
  i.set_quote("USDT");
  return i;
}

fob::common::v1::Instrument WethUsdc() {
  fob::common::v1::Instrument i;
  i.set_symbol("WETH/USDC");
  i.set_base("WETH");
  i.set_quote("USDC");
  return i;
}

ExecutionIntent MakeIntent(const std::string& id,
                           const fob::common::v1::Side side,
                           const int64_t qty_units,
                           const int32_t qty_scale,
                           const int64_t limit_units,
                           const int32_t price_scale,
                           const std::string& venue,
                           const fob::common::v1::Instrument& instrument) {
  ExecutionIntent intent;
  intent.set_intent_id(id);
  intent.set_client_order_id(id + "-c");
  intent.set_venue(venue);
  intent.set_venue_symbol(instrument.base() + instrument.quote());
  *intent.mutable_instrument() = instrument;
  intent.set_side(side);
  intent.mutable_target_qty()->set_units(qty_units);
  intent.mutable_target_qty()->set_scale(qty_scale);
  if (limit_units != 0) {
    intent.mutable_limit_price()->set_units(limit_units);
    intent.mutable_limit_price()->set_scale(price_scale);
  }
  return intent;
}

domain::VenueRawSnapshot MakeBtcSnapshot(
    const std::vector<std::pair<int64_t, int64_t>>& bids,
    const std::vector<std::pair<int64_t, int64_t>>& asks) {
  domain::VenueRawSnapshot s;
  s.venue_id = "binance";
  s.venue_type = domain::VenueType::kCex;
  s.instrument = BtcUsdt();
  s.venue_symbol = "BTCUSDT";
  s.status = domain::VenueConnectionStatus::kConnected;
  s.mid_price.units = 6800000;
  s.mid_price.scale = 2;
  for (const auto& [p, q] : bids) {
    domain::VenueBookLevel level;
    level.price.units = p;
    level.price.scale = 2;
    level.qty.units = q;
    level.qty.scale = 3;
    s.bids.push_back(level);
  }
  for (const auto& [p, q] : asks) {
    domain::VenueBookLevel level;
    level.price.units = p;
    level.price.scale = 2;
    level.qty.units = q;
    level.qty.scale = 3;
    s.asks.push_back(level);
  }
  if (!s.bids.empty()) s.best_bid = s.bids.front().price;
  if (!s.asks.empty()) s.best_ask = s.asks.front().price;
  return s;
}

std::string format_status(const ExecutionReportStatus s) {
  return std::to_string(static_cast<int>(s));
}

}  // namespace

const char* BacktestSyntheticScenarios::ToString(const ScenarioId id) {
  switch (id) {
    case ScenarioId::kPartialFill:
      return "partial_fill";
    case ScenarioId::kRejectionFallback:
      return "rejection_fallback";
    case ScenarioId::kOverfillRace:
      return "overfill_race";
    case ScenarioId::kVenueTimeout:
      return "venue_timeout";
    case ScenarioId::kDexGasSpike:
      return "dex_gas_spike";
    case ScenarioId::kCircuitBreakerOpen:
      return "circuit_breaker_open";
  }
  return "unknown";
}

void BacktestSyntheticScenarios::Scenario::ApplyTo(
    infra::VenueSimAdapter& adapter) const {
  if (snapshot.has_value()) {
    adapter.SetLastSnapshot(*snapshot);
  }
  if (configure) configure(adapter);
}

BacktestSyntheticScenarios::Scenario
BacktestSyntheticScenarios::PartialFill() {
  Scenario s;
  s.id = ScenarioId::kPartialFill;
  s.name = ToString(s.id);
  s.description =
      "Target qty exceeds visible ask liquidity. VenueSim must fill the "
      "available depth and report PARTIALLY_FILLED with remaining_qty.";
  s.venue_type = domain::VenueType::kCex;

  // Single ask level @ 68000.00 with 0.100 BTC depth.
  s.snapshot = MakeBtcSnapshot({}, {{6800000, 100}});

  s.intents = {
      MakeIntent("pf-1", fob::common::v1::SIDE_BUY,
                 /*qty_units=*/250, /*qty_scale=*/3,
                 /*limit_units=*/6800500, /*price_scale=*/2,
                 "binance", BtcUsdt()),
  };

  s.configure = [](infra::VenueSimAdapter& adapter) {
    adapter.Connect();
    infra::VenueSimAdapter::OrderPolicy policy;
    policy.walk_book = true;
    policy.enforce_limit_price = true;
    policy.taker_fee_bps = 10;
    adapter.SetDefaultOrderPolicy(policy);
  };

  s.expected = {
      ExpectedOutcome{
          .intent_index = 0,
          .intent_id = "pf-1",
          .expected_status = EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
          .min_filled_units = 100,
          .max_filled_units = 100,
      },
  };
  return s;
}

BacktestSyntheticScenarios::Scenario
BacktestSyntheticScenarios::RejectionFallback() {
  Scenario s;
  s.id = ScenarioId::kRejectionFallback;
  s.name = ToString(s.id);
  s.description =
      "Primary intent rejected by venue (e.g. INVALID_PRICE). Adapter must "
      "surface a REJECTED ExecutionReport with the reject code so Execution "
      "Planning can issue a fallback routing plan on the remaining qty.";
  s.venue_type = domain::VenueType::kCex;
  s.snapshot = MakeBtcSnapshot({}, {{6800000, 1000}});

  // rf-1 — rejected by venue.
  // rf-2 — fallback hedge on the same target qty, fills cleanly.
  s.intents = {
      MakeIntent("rf-1", fob::common::v1::SIDE_BUY,
                 /*qty_units=*/200, 3,
                 /*limit_units=*/6800500, 2,
                 "binance", BtcUsdt()),
      MakeIntent("rf-2", fob::common::v1::SIDE_BUY,
                 /*qty_units=*/200, 3,
                 /*limit_units=*/6801000, 2,
                 "binance", BtcUsdt()),
  };

  s.configure = [](infra::VenueSimAdapter& adapter) {
    adapter.Connect();
    infra::VenueSimAdapter::OrderPolicy default_policy;
    default_policy.walk_book = true;
    default_policy.enforce_limit_price = true;
    default_policy.taker_fee_bps = 10;
    adapter.SetDefaultOrderPolicy(default_policy);

    infra::VenueSimAdapter::OrderPolicy reject_policy = default_policy;
    reject_policy.reject_code = "INVALID_PRICE";
    reject_policy.reject_message = "VenueSim synthetic reject for fallback";
    adapter.SetOrderPolicyFor("rf-1", reject_policy);
  };

  s.expected = {
      ExpectedOutcome{
          .intent_index = 0,
          .intent_id = "rf-1",
          .expected_status = EXECUTION_REPORT_STATUS_REJECTED,
          .min_filled_units = 0,
          .max_filled_units = 0,
          .expected_error_code = "INVALID_PRICE",
      },
      ExpectedOutcome{
          .intent_index = 1,
          .intent_id = "rf-2",
          .expected_status = EXECUTION_REPORT_STATUS_FILLED,
          .min_filled_units = 200,
          .max_filled_units = 200,
      },
  };
  return s;
}

BacktestSyntheticScenarios::Scenario
BacktestSyntheticScenarios::OverfillRace() {
  Scenario s;
  s.id = ScenarioId::kOverfillRace;
  s.name = ToString(s.id);
  s.description =
      "Two child orders race on the same HedgeFlow. The book has enough "
      "depth to fully fill both, but their aggregate fill exceeds the "
      "HedgeFlow target — Venue Execution Adapter is expected to engage "
      "the overfill guard. From VenueSim's perspective both child orders "
      "return FILLED; the overfill guard is exercised in the harness/test "
      "that observes the accumulated filled_qty.";
  s.venue_type = domain::VenueType::kCex;
  // Deep enough book so both child orders fully fill.
  s.snapshot = MakeBtcSnapshot({}, {{6800000, 2000}});

  s.intents = {
      MakeIntent("of-a", fob::common::v1::SIDE_BUY,
                 /*qty_units=*/300, 3,
                 /*limit_units=*/6800500, 2,
                 "binance", BtcUsdt()),
      MakeIntent("of-b", fob::common::v1::SIDE_BUY,
                 /*qty_units=*/250, 3,
                 /*limit_units=*/6800500, 2,
                 "binance", BtcUsdt()),
  };

  s.configure = [](infra::VenueSimAdapter& adapter) {
    adapter.Connect();
    infra::VenueSimAdapter::OrderPolicy policy;
    policy.walk_book = true;
    policy.enforce_limit_price = true;
    policy.taker_fee_bps = 10;
    adapter.SetDefaultOrderPolicy(policy);
  };

  s.expected = {
      ExpectedOutcome{
          .intent_index = 0,
          .intent_id = "of-a",
          .expected_status = EXECUTION_REPORT_STATUS_FILLED,
          .min_filled_units = 300,
          .max_filled_units = 300,
      },
      ExpectedOutcome{
          .intent_index = 1,
          .intent_id = "of-b",
          .expected_status = EXECUTION_REPORT_STATUS_FILLED,
          .min_filled_units = 250,
          .max_filled_units = 250,
      },
  };
  return s;
}

BacktestSyntheticScenarios::Scenario
BacktestSyntheticScenarios::VenueTimeout() {
  Scenario s;
  s.id = ScenarioId::kVenueTimeout;
  s.name = ToString(s.id);
  s.description =
      "Venue does not acknowledge the order within hedgeTimeoutMs. "
      "VenueSim emulates the timeout by emitting a CANCELLED status with "
      "error_code=VENUE_TIMEOUT — Venue Execution Adapter is then expected "
      "to retry / hand the remainder to Execution Planning.";
  s.venue_type = domain::VenueType::kCex;
  s.snapshot = MakeBtcSnapshot({}, {{6800000, 500}});

  s.intents = {
      MakeIntent("vt-1", fob::common::v1::SIDE_BUY,
                 /*qty_units=*/150, 3,
                 /*limit_units=*/6800500, 2,
                 "binance", BtcUsdt()),
  };

  s.configure = [](infra::VenueSimAdapter& adapter) {
    adapter.Connect();
    infra::VenueSimAdapter::OrderPolicy timeout_policy;
    timeout_policy.walk_book = false;
    timeout_policy.fill_ratio = 0.0;
    timeout_policy.forced_status = EXECUTION_REPORT_STATUS_CANCELLED;
    // Reject path embeds the reason — VenueSim treats non-empty reject_code
    // as a hard REJECTED, so the harness uses forced_status + a synthetic
    // latency to surface the timeout instead.
    timeout_policy.latency_ms = 30000;
    adapter.SetDefaultOrderPolicy(timeout_policy);
  };

  s.expected = {
      ExpectedOutcome{
          .intent_index = 0,
          .intent_id = "vt-1",
          .expected_status = EXECUTION_REPORT_STATUS_CANCELLED,
          .min_filled_units = 0,
          .max_filled_units = 0,
      },
  };
  return s;
}

BacktestSyntheticScenarios::Scenario
BacktestSyntheticScenarios::DexGasSpike() {
  Scenario s;
  s.id = ScenarioId::kDexGasSpike;
  s.name = ToString(s.id);
  s.description =
      "DEX gas price spikes above the policy limit. The on-chain tx fails "
      "and VenueSim surfaces a REJECTED report with error_code=TX_FAILED. "
      "Reconciliation must record a full reconciliationGap for the intent.";
  s.venue_type = domain::VenueType::kDex;

  s.intents = {
      MakeIntent("gas-1", fob::common::v1::SIDE_BUY,
                 /*qty_units=*/500, 3,
                 /*limit_units=*/350000, 2,
                 "uniswap_v3", WethUsdc()),
  };

  s.configure = [](infra::VenueSimAdapter& adapter) {
    adapter.Connect();
    infra::VenueSimAdapter::OrderPolicy policy;
    policy.walk_book = false;
    policy.reject_code = "TX_FAILED";
    policy.reject_message = "DEX gas spike: tx reverted, gas > maxFeePerGas";
    adapter.SetDefaultOrderPolicy(policy);
  };

  s.expected = {
      ExpectedOutcome{
          .intent_index = 0,
          .intent_id = "gas-1",
          .expected_status = EXECUTION_REPORT_STATUS_REJECTED,
          .min_filled_units = 0,
          .max_filled_units = 0,
          .expected_error_code = "TX_FAILED",
      },
  };
  return s;
}

BacktestSyntheticScenarios::Scenario
BacktestSyntheticScenarios::CircuitBreakerOpen() {
  Scenario s;
  s.id = ScenarioId::kCircuitBreakerOpen;
  s.name = ToString(s.id);
  s.description =
      "Venue circuit breaker is open. VenueSim returns VENUE_UNAVAILABLE "
      "for every intent — Execution Planning must exclude the venue from "
      "subsequent routing.";
  s.venue_type = domain::VenueType::kCex;

  s.intents = {
      MakeIntent("cb-1", fob::common::v1::SIDE_BUY,
                 /*qty_units=*/120, 3,
                 /*limit_units=*/6800500, 2,
                 "binance", BtcUsdt()),
      MakeIntent("cb-2", fob::common::v1::SIDE_SELL,
                 /*qty_units=*/80, 3,
                 /*limit_units=*/6799500, 2,
                 "binance", BtcUsdt()),
  };

  s.configure = [](infra::VenueSimAdapter& adapter) {
    adapter.Connect();
    infra::VenueSimAdapter::OrderPolicy policy;
    policy.walk_book = false;
    policy.reject_code = "VENUE_UNAVAILABLE";
    policy.reject_message =
        "Circuit breaker OPEN for venue; reject all new child orders";
    adapter.SetDefaultOrderPolicy(policy);
  };

  s.expected = {
      ExpectedOutcome{
          .intent_index = 0,
          .intent_id = "cb-1",
          .expected_status = EXECUTION_REPORT_STATUS_REJECTED,
          .min_filled_units = 0,
          .max_filled_units = 0,
          .expected_error_code = "VENUE_UNAVAILABLE",
      },
      ExpectedOutcome{
          .intent_index = 1,
          .intent_id = "cb-2",
          .expected_status = EXECUTION_REPORT_STATUS_REJECTED,
          .min_filled_units = 0,
          .max_filled_units = 0,
          .expected_error_code = "VENUE_UNAVAILABLE",
      },
  };
  return s;
}

std::vector<BacktestSyntheticScenarios::Scenario>
BacktestSyntheticScenarios::All() {
  return {
      PartialFill(),
      RejectionFallback(),
      OverfillRace(),
      VenueTimeout(),
      DexGasSpike(),
      CircuitBreakerOpen(),
  };
}

std::vector<BacktestSyntheticScenarios::OutcomeIssue>
BacktestSyntheticScenarios::Verify(
    const Scenario& scenario,
    const std::vector<ExecutionReport>& reports) {
  std::vector<OutcomeIssue> issues;

  if (reports.size() != scenario.expected.size()) {
    issues.push_back(OutcomeIssue{
        .scenario_id = scenario.id,
        .intent_index = 0,
        .intent_id = "",
        .field = "report_count",
        .expected = std::to_string(scenario.expected.size()),
        .actual = std::to_string(reports.size()),
    });
    return issues;
  }

  for (std::size_t i = 0; i < scenario.expected.size(); ++i) {
    const auto& exp = scenario.expected[i];
    const auto& rep = reports[i];

    if (rep.status() != exp.expected_status) {
      issues.push_back(OutcomeIssue{
          .scenario_id = scenario.id,
          .intent_index = exp.intent_index,
          .intent_id = exp.intent_id,
          .field = "status",
          .expected = format_status(exp.expected_status),
          .actual = format_status(rep.status()),
      });
    }

    const int64_t filled = rep.filled_qty().units();
    if (exp.max_filled_units >= exp.min_filled_units) {
      if (filled < exp.min_filled_units || filled > exp.max_filled_units) {
        std::ostringstream want;
        want << "[" << exp.min_filled_units << "," << exp.max_filled_units
             << "]";
        issues.push_back(OutcomeIssue{
            .scenario_id = scenario.id,
            .intent_index = exp.intent_index,
            .intent_id = exp.intent_id,
            .field = "filled_qty.units",
            .expected = want.str(),
            .actual = std::to_string(filled),
        });
      }
    }

    if (!exp.expected_error_code.empty()) {
      const std::string& code = rep.error().code();
      if (code != exp.expected_error_code) {
        issues.push_back(OutcomeIssue{
            .scenario_id = scenario.id,
            .intent_index = exp.intent_index,
            .intent_id = exp.intent_id,
            .field = "error.code",
            .expected = exp.expected_error_code,
            .actual = code,
        });
      }
    }
  }

  return issues;
}

}  // namespace cex::venues::app
