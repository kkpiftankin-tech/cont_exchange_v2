#include "app/execution_intent_builder.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "cex/common/decimal.hpp"
#include "cex/common/time.hpp"

namespace cex::matching::app {

namespace {

constexpr int32_t kDecimalScale = 12;

fob::common::v1::Decimal DoubleToDecimal(const double value) {
  fob::common::v1::Decimal decimal;
  if (!std::isfinite(value)) {
    decimal.set_units(0);
    decimal.set_scale(kDecimalScale);
    return decimal;
  }

  const double factor = std::pow(10.0, static_cast<double>(kDecimalScale));
  decimal.set_units(static_cast<int64_t>(std::llround(value * factor)));
  decimal.set_scale(kDecimalScale);
  return decimal;
}

fob::common::v1::Side ToProtoSide(const ForecastSide side) {
  switch (side) {
    case ForecastSide::kBuy:
      return fob::common::v1::SIDE_BUY;
    case ForecastSide::kSell:
      return fob::common::v1::SIDE_SELL;
  }
  return fob::common::v1::SIDE_UNSPECIFIED;
}

void PopulateInstrument(const std::string& symbol,
                        fob::common::v1::Instrument* instrument) {
  if (instrument == nullptr) return;

  instrument->set_symbol(symbol);

  const std::size_t separator = symbol.find('/');
  if (separator == std::string::npos ||
      separator == 0 ||
      separator + 1 >= symbol.size()) {
    return;
  }

  instrument->set_base(symbol.substr(0, separator));
  instrument->set_quote(symbol.substr(separator + 1));
}

std::string MakeIntentId(const std::string& batch_id,
                         const std::string& order_id,
                         const std::string& symbol,
                         const std::string& venue_id) {
  return batch_id + "|" + order_id + "|" + symbol + "|" + venue_id;
}

std::string MakeExternalFillIntentId(const std::string& batch_id,
                                     const std::string& order_id,
                                     const std::string& symbol,
                                     const std::string& venue_id,
                                     const int fill_index) {
  return batch_id + "|" + order_id + "|" + symbol + "|" + venue_id +
         "|external_fill_" + std::to_string(fill_index);
}

bool IsExternalFill(const fob::matching::v1::FlowFill& fill) {
  const std::string source = fill.liquidity_source();
  if (source.empty() || source == "internal") return false;
  if (fill.provenance().venue_id().empty()) return false;
  if (fill.side() == fob::common::v1::SIDE_UNSPECIFIED) return false;
  const auto qty = cex::common::Decimal::from_proto(fill.executed_qty());
  return static_cast<double>(qty) > 0.0;
}

cex::common::Decimal AbsDecimal(const cex::common::Decimal& value) {
  if (cex::common::Decimal::cmp(value, cex::common::Decimal::zero()) >= 0) {
    return value;
  }
  return cex::common::Decimal::sub(cex::common::Decimal::zero(), value);
}

bool HasValidReferenceMid(const fob::common::v1::Decimal& reference_mid) {
  return cex::common::Decimal::cmp(
             cex::common::Decimal::from_proto(reference_mid),
             cex::common::Decimal::zero()) > 0;
}

std::string MakeHedgeFlowId(const PositionSnapshot& snapshot) {
  return snapshot.batch_id + "|hedge|" + snapshot.provider_id + "|" + snapshot.symbol;
}

std::optional<fob::execution::v1::ExecutionIntent> BuildIntentForLeg(
    const ExecutionIntentBuildRequest& request,
    const OrderLegForecast& leg_forecast,
    const VenueComparison& best_comparison,
    const std::optional<double> limit_price) {
  if (best_comparison.venue_id.empty()) return std::nullopt;
  if (!std::isfinite(leg_forecast.requested_qty) || leg_forecast.requested_qty <= 0.0) {
    return std::nullopt;
  }

  fob::execution::v1::ExecutionIntent intent;
  const std::string intent_id = MakeIntentId(
      request.batch_id,
      request.order_id,
      leg_forecast.symbol,
      best_comparison.venue_id);

  auto* meta = intent.mutable_meta();
  meta->set_event_id(intent_id);
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("matching");
  meta->set_correlation_id(request.batch_id);
  meta->set_partition_key(best_comparison.venue_id + "|" + leg_forecast.symbol);

  intent.set_intent_id(intent_id);
  intent.set_batch_id(request.batch_id);
  intent.set_internal_order_id(request.order_id);
  intent.set_reason("hedge");
  intent.set_venue(best_comparison.venue_id);
  PopulateInstrument(leg_forecast.symbol, intent.mutable_instrument());
  intent.set_side(ToProtoSide(leg_forecast.side));
  *intent.mutable_target_qty() = DoubleToDecimal(leg_forecast.requested_qty);
  intent.set_client_order_id(intent_id);

  if (limit_price.has_value() &&
      std::isfinite(*limit_price) &&
      *limit_price > 0.0) {
    *intent.mutable_limit_price() = DoubleToDecimal(*limit_price);
  }

  return intent;
}

std::optional<fob::execution::v1::ExecutionIntent> BuildIntentForExternalFill(
    const fob::matching::v1::BatchResult& batch,
    const fob::matching::v1::FlowFill& fill,
    const int fill_index) {
  if (!IsExternalFill(fill)) return std::nullopt;

  std::string symbol = fill.instrument().symbol();
  if (symbol.empty() &&
      !fill.instrument().base().empty() &&
      !fill.instrument().quote().empty()) {
    symbol = fill.instrument().base() + "/" + fill.instrument().quote();
  }
  if (symbol.empty()) return std::nullopt;

  const std::string& venue_id = fill.provenance().venue_id();
  const std::string intent_id = MakeExternalFillIntentId(
      batch.batch_id(),
      fill.order_id(),
      symbol,
      venue_id,
      fill_index);

  fob::execution::v1::ExecutionIntent intent;
  auto* meta = intent.mutable_meta();
  meta->set_event_id(intent_id);
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("matching");
  meta->set_correlation_id(batch.batch_id());
  meta->set_partition_key(venue_id + "|" + symbol);

  intent.set_intent_id(intent_id);
  intent.set_batch_id(batch.batch_id());
  intent.set_internal_order_id(fill.order_id());
  intent.set_reason("external_liquidity");
  intent.set_venue(venue_id);
  *intent.mutable_instrument() = fill.instrument();
  PopulateInstrument(symbol, intent.mutable_instrument());
  intent.set_side(fill.side());
  *intent.mutable_target_qty() = fill.executed_qty();
  *intent.mutable_limit_price() = fill.price();
  intent.set_strategy(fob::execution::v1::EXEC_STRATEGY_LIMIT);
  intent.set_urgency(fob::execution::v1::URGENCY_HIGH);
  intent.set_tif(fob::common::v1::TIF_IOC);
  intent.set_client_order_id(intent_id);

  return intent;
}

}  // namespace

ExecutionIntentBuildResult ExecutionIntentBuilder::Build(
    const ExecutionIntentBuildRequest& request) const {
  ExecutionIntentBuildResult result;
  result.leg_decisions.reserve(request.order_forecast.leg_forecasts.size());

  for (const auto& leg_forecast : request.order_forecast.leg_forecasts) {
    ExecutionIntentLegDecision decision;
    decision.symbol = leg_forecast.symbol;
    decision.side = leg_forecast.side;
    decision.requested_qty = leg_forecast.requested_qty;

    if (!request.order_forecast.feasible) {
      decision.reason = request.order_forecast.reject_reason.empty()
          ? "order_infeasible"
          : request.order_forecast.reject_reason;
      result.leg_decisions.push_back(std::move(decision));
      continue;
    }

    if (!leg_forecast.feasible) {
      decision.reason = leg_forecast.reject_reason.empty()
          ? "leg_infeasible"
          : leg_forecast.reject_reason;
      result.leg_decisions.push_back(std::move(decision));
      continue;
    }

    if (!leg_forecast.best_comparison.has_value()) {
      decision.reason = "best_comparison_missing";
      result.leg_decisions.push_back(std::move(decision));
      continue;
    }

    const auto& best_comparison = *leg_forecast.best_comparison;
    if (!best_comparison.feasible) {
      decision.reason = best_comparison.reject_reason.empty()
          ? "leg_infeasible"
          : best_comparison.reject_reason;
      result.leg_decisions.push_back(std::move(decision));
      continue;
    }

    const std::optional<double> limit_price = leg_forecast.side == ForecastSide::kBuy
        ? request.buy_limit_price
        : request.sell_limit_price;

    const auto maybe_intent = BuildIntentForLeg(
        request,
        leg_forecast,
        best_comparison,
        limit_price);
    if (!maybe_intent.has_value()) {
      decision.reason = "intent_build_failed";
      result.leg_decisions.push_back(std::move(decision));
      continue;
    }

    decision.publishable = true;
    decision.selected_venue = best_comparison.venue_id;
    decision.has_limit_price = limit_price.has_value();
    decision.expected_vwap = best_comparison.expected_vwap;
    decision.expected_is_bps = best_comparison.expected_is_bps;
    result.leg_decisions.push_back(std::move(decision));
    result.intents.push_back(*maybe_intent);
  }

  return result;
}

ExecutionIntentBuildResult ExecutionIntentBuilder::BuildFromExternalFills(
    const fob::matching::v1::BatchResult& batch) const {
  ExecutionIntentBuildResult result;
  result.intents.reserve(static_cast<std::size_t>(batch.fills_size()));

  for (int i = 0; i < batch.fills_size(); ++i) {
    const auto maybe_intent = BuildIntentForExternalFill(batch, batch.fills(i), i);
    if (maybe_intent.has_value()) {
      result.intents.push_back(*maybe_intent);
    }
  }

  return result;
}

HedgeExecutionIntentBuildResult ExecutionIntentBuilder::BuildFromHedgeTriggerDecisions(
    const std::vector<HedgeTriggerDecision>& decisions,
    const HedgeExecutionIntentConfig& config) const {
  HedgeExecutionIntentBuildResult result;
  result.decisions.reserve(decisions.size());
  result.intents.reserve(decisions.size());

  for (const auto& decision : decisions) {
    HedgeExecutionIntentBuildDecision build_decision;
    build_decision.snapshot = decision.snapshot;
    build_decision.triggered = decision.triggered;

    if (!decision.triggered) {
      build_decision.reason = "not_triggered";
      result.decisions.push_back(std::move(build_decision));
      continue;
    }

    if (!HasValidReferenceMid(decision.snapshot.clearing_price)) {
      build_decision.reason = "reference_mid_non_positive";
      result.decisions.push_back(std::move(build_decision));
      continue;
    }

    const cex::common::Decimal net_qty = decision.snapshot.net_qty;
    const cex::common::Decimal zero = cex::common::Decimal::zero();
    const int net_qty_cmp = cex::common::Decimal::cmp(net_qty, zero);
    if (net_qty_cmp == 0) {
      build_decision.reason = "zero_net_qty";
      result.decisions.push_back(std::move(build_decision));
      continue;
    }

    const auto target_qty = AbsDecimal(net_qty);
    const auto reference_mid = cex::common::Decimal::from_proto(decision.snapshot.clearing_price);
    const auto target_notional = cex::common::Decimal::mul(target_qty, reference_mid);

    const std::string hedge_flow_id = MakeHedgeFlowId(decision.snapshot);
    const std::string intent_id = hedge_flow_id + "|intent";

    fob::execution::v1::ExecutionIntent intent;
    auto* meta = intent.mutable_meta();
    meta->set_event_id(intent_id);
    *meta->mutable_ts_event() = cex::common::now_ts();
    meta->set_source("matching");
    meta->set_correlation_id(decision.snapshot.batch_id);
    meta->set_partition_key(decision.snapshot.provider_id + "|" + decision.snapshot.symbol);

    intent.set_intent_id(intent_id);
    intent.set_hedge_flow_id(hedge_flow_id);
    intent.set_batch_id(decision.snapshot.batch_id);
    intent.set_provider_id(decision.snapshot.provider_id);
    intent.set_source(fob::execution::v1::HEDGE_SOURCE_AUTO_BATCH);
    intent.set_reason("hedge");
    PopulateInstrument(decision.snapshot.symbol, intent.mutable_instrument());
    intent.set_side(
        net_qty_cmp > 0 ? fob::common::v1::SIDE_SELL : fob::common::v1::SIDE_BUY);
    *intent.mutable_target_qty() = target_qty.to_proto();
    *intent.mutable_target_notional() = target_notional.to_proto();
    *intent.mutable_reference_mid() = decision.snapshot.clearing_price;

    intent.set_strategy(config.strategy);
    intent.set_urgency(config.urgency);
    intent.set_tif(config.tif);
    intent.set_timeout_ms(config.timeout_ms > 0 ? config.timeout_ms : 30000);
    intent.set_max_slippage_bps(config.max_slippage_bps);
    for (const auto& venue : config.allowed_venues) {
      intent.add_allowed_venues(venue);
    }
    intent.set_client_order_id(intent_id);

    build_decision.intent_created = true;
    build_decision.reason = "built";
    result.decisions.push_back(std::move(build_decision));
    result.intents.push_back(std::move(intent));
  }

  return result;
}

}  // namespace cex::matching::app
