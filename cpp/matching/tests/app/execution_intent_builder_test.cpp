#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "app/execution_intent_builder.hpp"
#include "app/hedge_execution_intents_publisher.hpp"
#include "app/planner_inputs_cache.hpp"
#include "cex/common/decimal.hpp"
#include "cex/common/proto.hpp"
#include "infra/kafka/execution_intents_producer.hpp"

namespace {

using cex::common::Decimal;
using cex::matching::app::ExecutionIntentBuildRequest;
using cex::matching::app::ExecutionIntentBuilder;
using cex::matching::app::ForecastSide;
using cex::matching::app::HedgeExecutionIntentsPublishResult;
using cex::matching::app::HedgeExecutionIntentConfig;
using cex::matching::app::HedgeTriggerDecision;
using cex::matching::app::OrderForecast;
using cex::matching::app::OrderForecastRequest;
using cex::matching::app::OrderLegForecast;
using cex::matching::app::OrderLegRequest;
using cex::matching::app::PlannerInputsCache;
using cex::matching::app::PublishAutoHedgeExecutionIntents;
using cex::matching::app::VenueComparison;
using cex::matching::infra::ExecutionIntentsProducer;

constexpr double kTolerance = 1e-9;

struct ProducedRecord {
  std::string topic;
  std::string key;
  std::string payload;
};

bool Expect(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << '\n';
  return false;
}

bool ExpectNear(const double actual,
                const double expected,
                const std::string& message) {
  if (std::abs(actual - expected) <= kTolerance) return true;
  std::cerr << "[FAIL] " << message
            << " expected=" << expected
            << " actual=" << actual << '\n';
  return false;
}

VenueComparison MakeBestComparison(const std::string& venue,
                                   const std::string& symbol,
                                   const ForecastSide side,
                                   const double requested_qty,
                                   const double expected_vwap,
                                   const double expected_is_bps) {
  VenueComparison comparison;
  comparison.venue_id = venue;
  comparison.symbol = symbol;
  comparison.side = side;
  comparison.requested_qty = requested_qty;
  comparison.expected_vwap = expected_vwap;
  comparison.expected_is_bps = expected_is_bps;
  comparison.feasible = true;
  return comparison;
}

OrderLegForecast MakeLegForecast(const std::string& symbol,
                                 const ForecastSide side,
                                 const double requested_qty,
                                 const bool feasible,
                                 const std::optional<VenueComparison>& best_comparison,
                                 const std::string& reject_reason = "") {
  OrderLegForecast leg;
  leg.symbol = symbol;
  leg.side = side;
  leg.requested_qty = requested_qty;
  leg.requested_speed = requested_qty;
  leg.feasible = feasible;
  leg.reject_reason = reject_reason;
  if (best_comparison.has_value()) {
    leg.best_comparison = *best_comparison;
    leg.comparisons.push_back(*best_comparison);
  }
  return leg;
}

OrderForecast MakeForecast(const std::string& order_id, const bool feasible) {
  OrderForecast forecast;
  forecast.order_id = order_id;
  forecast.requested_qty = 1.0;
  forecast.requested_speed = 1.0;
  forecast.feasible = feasible;
  if (!feasible) {
    forecast.reject_reason = "leg_infeasible";
  }
  return forecast;
}

void FillSideCurve(fob::venue::v1::SideLiquidityCurve* side,
                   const std::vector<double>& q_grid,
                   const std::vector<double>& p_of_q,
                   const std::vector<double>& s_of_q) {
  side->clear_q_grid();
  side->clear_p_of_q();
  side->clear_s_of_q();
  for (double value : q_grid) side->add_q_grid(value);
  for (double value : p_of_q) side->add_p_of_q(value);
  for (double value : s_of_q) side->add_s_of_q(value);
}

fob::venue::v1::VenueLiquidityCurve MakeCurve(const std::string& venue,
                                              const std::string& symbol,
                                              const double ask_notional) {
  fob::venue::v1::VenueLiquidityCurve curve;
  curve.set_venue_id(venue);
  curve.mutable_instrument()->set_symbol(symbol);
  curve.set_confidence(1.0);
  curve.set_tau_ms(1000.0);
  curve.set_level("L1");

  FillSideCurve(curve.mutable_ask_curve(), {1.0}, {ask_notional}, {ask_notional});
  FillSideCurve(curve.mutable_bid_curve(), {1.0}, {ask_notional}, {ask_notional});
  return curve;
}

HedgeTriggerDecision MakeHedgeDecision(const std::string& batch_id,
                                       const std::string& provider_id,
                                       const std::string& symbol,
                                       const int64_t net_qty_units,
                                       const int32_t net_qty_scale,
                                       const int64_t clearing_price_units,
                                       const int32_t clearing_price_scale,
                                       const bool triggered) {
  HedgeTriggerDecision decision;
  decision.snapshot.batch_id = batch_id;
  decision.snapshot.provider_id = provider_id;
  decision.snapshot.symbol = symbol;
  decision.snapshot.net_qty = Decimal{net_qty_units, net_qty_scale};
  decision.snapshot.clearing_price.set_units(clearing_price_units);
  decision.snapshot.clearing_price.set_scale(clearing_price_scale);
  decision.triggered = triggered;
  return decision;
}

bool TestMapsBuyLegToExecutionIntent() {
  ExecutionIntentBuilder builder;
  auto forecast = MakeForecast("order-1", true);
  forecast.leg_forecasts.push_back(MakeLegForecast(
      "BTC/USDT",
      ForecastSide::kBuy,
      1.25,
      true,
      MakeBestComparison("binance", "BTC/USDT", ForecastSide::kBuy, 1.25, 101.5, 12.3)));

  ExecutionIntentBuildRequest request;
  request.batch_id = "batch-1";
  request.order_id = "order-1";
  request.order_forecast = forecast;
  request.buy_limit_price = 102.0;
  request.sell_limit_price = 97.0;

  const auto result = builder.Build(request);

  bool ok = true;
  ok = Expect(result.intents.size() == 1, "BUY leg must produce one intent") && ok;
  if (result.intents.size() != 1) return false;

  const auto& intent = result.intents.front();
  ok = Expect(intent.venue() == "binance", "BUY intent must use selected venue") && ok;
  ok = Expect(intent.side() == fob::common::v1::SIDE_BUY, "BUY intent side mapping") && ok;
  ok = Expect(intent.batch_id() == "batch-1", "BUY intent batch_id mapping") && ok;
  ok = Expect(intent.internal_order_id() == "order-1", "BUY intent order_id mapping") && ok;
  ok = Expect(intent.reason() == "hedge", "BUY intent reason mapping") && ok;
  ok = Expect(intent.has_limit_price(), "BUY intent must include buy limit") && ok;
  ok = ExpectNear(static_cast<double>(Decimal::from_proto(intent.target_qty())), 1.25,
                  "BUY intent target_qty mapping") && ok;
  ok = ExpectNear(static_cast<double>(Decimal::from_proto(intent.limit_price())), 102.0,
                  "BUY intent limit_price mapping") && ok;
  ok = Expect(intent.instrument().symbol() == "BTC/USDT", "BUY instrument symbol mapping") && ok;
  ok = Expect(intent.instrument().base() == "BTC", "BUY instrument base mapping") && ok;
  ok = Expect(intent.instrument().quote() == "USDT", "BUY instrument quote mapping") && ok;
  return ok;
}

bool TestMapsSellLegToExecutionIntent() {
  ExecutionIntentBuilder builder;
  auto forecast = MakeForecast("order-2", true);
  forecast.leg_forecasts.push_back(MakeLegForecast(
      "ETH/USDT",
      ForecastSide::kSell,
      2.0,
      true,
      MakeBestComparison("kraken", "ETH/USDT", ForecastSide::kSell, 2.0, 1999.0, 8.5)));

  ExecutionIntentBuildRequest request;
  request.batch_id = "batch-2";
  request.order_id = "order-2";
  request.order_forecast = forecast;
  request.buy_limit_price = 2100.0;
  request.sell_limit_price = 1990.0;

  const auto result = builder.Build(request);

  bool ok = true;
  ok = Expect(result.intents.size() == 1, "SELL leg must produce one intent") && ok;
  if (result.intents.size() != 1) return false;

  const auto& intent = result.intents.front();
  ok = Expect(intent.venue() == "kraken", "SELL intent venue mapping") && ok;
  ok = Expect(intent.side() == fob::common::v1::SIDE_SELL, "SELL intent side mapping") && ok;
  ok = Expect(intent.has_limit_price(), "SELL intent must include sell limit") && ok;
  ok = ExpectNear(static_cast<double>(Decimal::from_proto(intent.limit_price())), 1990.0,
                  "SELL intent limit_price mapping") && ok;
  ok = Expect(intent.instrument().base() == "ETH", "SELL instrument base mapping") && ok;
  ok = Expect(intent.instrument().quote() == "USDT", "SELL instrument quote mapping") && ok;
  return ok;
}

bool TestInfeasibleLegDoesNotProduceIntent() {
  ExecutionIntentBuilder builder;
  auto forecast = MakeForecast("order-3", true);
  forecast.leg_forecasts.push_back(MakeLegForecast(
      "SOL/USDT",
      ForecastSide::kBuy,
      3.0,
      false,
      std::nullopt,
      "no_venues"));

  ExecutionIntentBuildRequest request;
  request.batch_id = "batch-3";
  request.order_id = "order-3";
  request.order_forecast = forecast;
  request.buy_limit_price = 110.0;

  const auto result = builder.Build(request);

  bool ok = true;
  ok = Expect(result.intents.empty(), "Infeasible leg must not produce intent") && ok;
  ok = Expect(result.leg_decisions.size() == 1, "Infeasible leg decision emitted") && ok;
  if (result.leg_decisions.size() != 1) return false;
  ok = Expect(!result.leg_decisions[0].publishable, "Infeasible leg decision is skipped") && ok;
  ok = Expect(result.leg_decisions[0].reason == "no_venues", "Infeasible leg keeps reject reason") && ok;
  return ok;
}

bool TestInfeasibleOrderDoesNotProduceAnyIntent() {
  ExecutionIntentBuilder builder;
  auto forecast = MakeForecast("order-3b", false);
  forecast.reject_reason = "BTC/USDT:no_venues";
  forecast.leg_forecasts.push_back(MakeLegForecast(
      "BTC/USDT",
      ForecastSide::kBuy,
      1.0,
      true,
      MakeBestComparison("binance", "BTC/USDT", ForecastSide::kBuy, 1.0, 100.0, 0.0)));

  ExecutionIntentBuildRequest request;
  request.batch_id = "batch-3b";
  request.order_id = "order-3b";
  request.order_forecast = forecast;

  const auto result = builder.Build(request);

  bool ok = true;
  ok = Expect(result.intents.empty(), "Infeasible order must not produce intents") && ok;
  ok = Expect(result.leg_decisions.size() == 1, "Infeasible order emits leg decision") && ok;
  if (result.leg_decisions.size() != 1) return false;
  ok = Expect(!result.leg_decisions[0].publishable, "Infeasible order decision is skipped") && ok;
  ok = Expect(result.leg_decisions[0].reason == "BTC/USDT:no_venues",
              "Infeasible order skip reason propagated") && ok;
  return ok;
}

bool TestDeterministicIntentAndClientOrderId() {
  ExecutionIntentBuilder builder;
  auto forecast = MakeForecast("order-4", true);
  forecast.leg_forecasts.push_back(MakeLegForecast(
      "BTC/USDT",
      ForecastSide::kBuy,
      0.5,
      true,
      MakeBestComparison("okx", "BTC/USDT", ForecastSide::kBuy, 0.5, 100.0, 0.0)));

  ExecutionIntentBuildRequest request;
  request.batch_id = "batch-4";
  request.order_id = "order-4";
  request.order_forecast = forecast;

  const auto first = builder.Build(request);
  const auto second = builder.Build(request);

  bool ok = true;
  ok = Expect(first.intents.size() == 1, "First deterministic build produces intent") && ok;
  ok = Expect(second.intents.size() == 1, "Second deterministic build produces intent") && ok;
  if (first.intents.size() != 1 || second.intents.size() != 1) return false;

  const std::string expected_id = "batch-4|order-4|BTC/USDT|okx";
  ok = Expect(first.intents[0].intent_id() == expected_id, "intent_id format must be deterministic") && ok;
  ok = Expect(second.intents[0].intent_id() == expected_id, "intent_id must be stable across builds") && ok;
  ok = Expect(first.intents[0].client_order_id() == expected_id, "client_order_id follows intent_id") && ok;
  return ok;
}

bool TestBestFeasibleComparisonVenueIsSelected() {
  cex::matching::app::VenueThresholds thresholds;
  thresholds.min_health_score = 0.5;
  thresholds.min_confidence   = 0.2;
  thresholds.confidence_for_l3    = 0.8;
  thresholds.confidence_for_l2    = 0.5;

  PlannerInputsCache cache(thresholds);
  cache.UpsertCurve(MakeCurve("alpha", "BTC/USDT", 102.0));
  cache.UpsertCurve(MakeCurve("beta", "BTC/USDT", 101.0));

  OrderForecastRequest request;
  request.order_id = "order-5";
  request.target_qty = 1.0;
  request.target_speed = 1.0;
  request.reference_prices_by_symbol["BTC/USDT"] = 100.0;
  request.legs.push_back(OrderLegRequest{
      .symbol = "BTC/USDT",
      .coefficient = 1.0,
  });

  const auto forecast = cache.BuildOrderForecast(request);

  bool ok = true;
  ok = Expect(forecast.feasible, "Planner forecast should be feasible with two venues") && ok;
  ok = Expect(forecast.leg_forecasts.size() == 1, "Planner forecast has one leg") && ok;
  if (forecast.leg_forecasts.size() != 1) return false;
  ok = Expect(forecast.leg_forecasts[0].best_comparison.has_value(),
              "Planner forecast must contain best comparison") && ok;
  if (!forecast.leg_forecasts[0].best_comparison.has_value()) return false;
  ok = Expect(forecast.leg_forecasts[0].best_comparison->venue_id == "beta",
              "Planner must choose best feasible venue by expected IS") && ok;

  ExecutionIntentBuilder builder;
  ExecutionIntentBuildRequest build_request;
  build_request.batch_id = "batch-5";
  build_request.order_id = "order-5";
  build_request.order_forecast = forecast;

  const auto build_result = builder.Build(build_request);
  ok = Expect(build_result.intents.size() == 1, "Selected planner venue must produce one intent") && ok;
  if (build_result.intents.size() != 1) return false;
  ok = Expect(build_result.intents[0].venue() == "beta",
              "Execution intent venue must match planner best feasible venue") && ok;
  return ok;
}

bool TestBuildsIocIntentFromExternalFill() {
  fob::matching::v1::BatchResult batch;
  batch.set_batch_id("batch-ext");

  auto* internal = batch.add_fills();
  internal->set_order_id("order-internal");
  internal->mutable_instrument()->set_symbol("BTC/USDT");
  internal->set_side(fob::common::v1::SIDE_BUY);
  internal->set_liquidity_source("internal");
  internal->mutable_executed_qty()->set_units(1);

  auto* external = batch.add_fills();
  external->set_order_id("order-ext");
  external->set_user_id("user-ext");
  external->mutable_instrument()->set_symbol("BTC/USDT");
  external->set_side(fob::common::v1::SIDE_BUY);
  external->set_liquidity_source("cex_hedge");
  external->mutable_provenance()->set_venue_id("binance");
  external->mutable_provenance()->set_snapshot_id("snapshot-binance");
  external->mutable_provenance()->set_curve_id("curve-binance");
  external->mutable_executed_qty()->set_units(125);
  external->mutable_executed_qty()->set_scale(2);
  external->mutable_price()->set_units(10025);
  external->mutable_price()->set_scale(2);

  ExecutionIntentBuilder builder;
  const auto result = builder.BuildFromExternalFills(batch);

  bool ok = true;
  ok = Expect(result.intents.size() == 1,
              "Only external fill must produce execution intent") && ok;
  if (result.intents.size() != 1) return false;

  const auto& intent = result.intents.front();
  ok = Expect(intent.intent_id() ==
                  "batch-ext|order-ext|BTC/USDT|binance|external_fill_1",
              "External fill intent id includes fill index") && ok;
  ok = Expect(intent.batch_id() == "batch-ext", "External fill batch_id mapping") && ok;
  ok = Expect(intent.internal_order_id() == "order-ext",
              "External fill order_id mapping") && ok;
  ok = Expect(intent.reason() == "external_liquidity",
              "External fill reason mapping") && ok;
  ok = Expect(intent.venue() == "binance", "External fill venue mapping") && ok;
  ok = Expect(intent.instrument().symbol() == "BTC/USDT",
              "External fill symbol mapping") && ok;
  ok = Expect(intent.instrument().base() == "BTC",
              "External fill base mapping") && ok;
  ok = Expect(intent.instrument().quote() == "USDT",
              "External fill quote mapping") && ok;
  ok = Expect(intent.side() == fob::common::v1::SIDE_BUY,
              "External fill side mapping") && ok;
  ok = Expect(intent.target_qty().units() == 125 && intent.target_qty().scale() == 2,
              "External fill target qty mapping") && ok;
  ok = Expect(intent.limit_price().units() == 10025 && intent.limit_price().scale() == 2,
              "External fill limit price mapping") && ok;
  ok = Expect(intent.strategy() == fob::execution::v1::EXEC_STRATEGY_LIMIT,
              "External fill strategy must be limit") && ok;
  ok = Expect(intent.urgency() == fob::execution::v1::URGENCY_HIGH,
              "External fill urgency must be high") && ok;
  ok = Expect(intent.tif() == fob::common::v1::TIF_IOC,
              "External fill intent must be IOC") && ok;
  ok = Expect(intent.client_order_id() == intent.intent_id(),
              "External fill client order id follows intent id") && ok;
  return ok;
}

bool TestPublisherWritesExecutionIntentsTopic() {
  std::vector<ProducedRecord> records;
  ExecutionIntentsProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back({topic, key, payload});
        return true;
      });

  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id("intent-publish");
  intent.mutable_meta()->set_partition_key("binance|BTC/USDT");
  intent.set_batch_id("batch-6");
  intent.mutable_instrument()->set_symbol("BTC/USDT");
  intent.set_venue("binance");

  bool ok = true;
  ok = Expect(producer.produce(intent), "Execution intent producer must succeed") && ok;
  ok = Expect(records.size() == 1, "Execution intent producer must emit one record") && ok;
  if (records.size() != 1) return false;

  ok = Expect(records[0].topic == "execution.intents", "Execution intent topic mapping") && ok;
  ok = Expect(records[0].key == "binance|BTC/USDT", "Execution intent key mapping") && ok;

  fob::execution::v1::ExecutionIntent parsed;
  const bool parsed_ok = cex::common::from_bytes(records[0].payload, parsed);
  ok = Expect(parsed_ok,
              "Execution intent payload must parse as protobuf") && ok;
  if (!parsed_ok) return false;
  ok = Expect(parsed.intent_id() == "intent-publish", "Execution intent payload content") && ok;
  return ok;
}

bool TestAutoHedgePublisherUsesHedgeFlowIdAsKey() {
  std::vector<ProducedRecord> records;
  ExecutionIntentsProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back({topic, key, payload});
        return true;
      });

  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id("intent-auto-1");
  intent.set_hedge_flow_id("hedge-flow-1");
  intent.set_batch_id("batch-auto-1");
  intent.set_provider_id("provider-1");
  intent.mutable_instrument()->set_symbol("BTC/USDT");

  bool ok = true;
  ok = Expect(producer.produce_auto_hedge(intent), "Auto-hedge producer must succeed") && ok;
  ok = Expect(records.size() == 1, "Auto-hedge producer emits one record") && ok;
  if (records.size() != 1) return false;

  ok = Expect(records[0].topic == "execution.intents", "Auto-hedge topic mapping") && ok;
  ok = Expect(records[0].key == "hedge-flow-1", "Auto-hedge key must be hedge_flow_id") && ok;

  fob::execution::v1::ExecutionIntent parsed;
  const bool parsed_ok = cex::common::from_bytes(records[0].payload, parsed);
  ok = Expect(parsed_ok, "Auto-hedge payload must parse as protobuf") && ok;
  if (!parsed_ok) return false;
  ok = Expect(parsed.hedge_flow_id() == "hedge-flow-1",
              "Auto-hedge payload hedge_flow_id must round-trip") && ok;
  return ok;
}

bool TestAutoHedgePublisherDeduplicatesVectorByHedgeFlowId() {
  std::vector<ProducedRecord> records;
  ExecutionIntentsProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back({topic, key, payload});
        return true;
      });

  fob::execution::v1::ExecutionIntent first;
  first.set_intent_id("intent-auto-dedup-1");
  first.set_hedge_flow_id("hedge-flow-dedup");
  first.set_batch_id("batch-auto-dedup");
  first.set_provider_id("provider-2");
  first.mutable_instrument()->set_symbol("ETH/USDT");

  auto second = first;
  second.set_intent_id("intent-auto-dedup-2");

  bool ok = true;
  ok = Expect(producer.produce_auto_hedge({first, second}),
              "Auto-hedge vector publish must succeed with duplicates") && ok;
  ok = Expect(records.size() == 1,
              "Auto-hedge vector publish must skip duplicate hedge_flow_id") && ok;
  if (records.size() != 1) return false;
  ok = Expect(records[0].key == "hedge-flow-dedup",
              "Deduplicated auto-hedge key stays hedge_flow_id") && ok;
  return ok;
}

bool TestAutoHedgePublishHelperRoutesIntentsToProducer() {
  std::vector<ProducedRecord> records;
  ExecutionIntentsProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back({topic, key, payload});
        return true;
      });

  fob::execution::v1::ExecutionIntent first;
  first.set_intent_id("intent-auto-helper-1");
  first.set_hedge_flow_id("hedge-flow-helper");
  first.set_batch_id("batch-auto-helper");
  first.set_provider_id("provider-helper");
  first.mutable_instrument()->set_symbol("SOL/USDT");

  auto second = first;
  second.set_intent_id("intent-auto-helper-2");

  const HedgeExecutionIntentsPublishResult result =
      PublishAutoHedgeExecutionIntents("batch-auto-helper", {first, second}, producer);

  bool ok = true;
  ok = Expect(result.attempted == 2, "Helper attempted count includes all intents") && ok;
  ok = Expect(result.published == 1, "Helper publishes only unique hedge_flow_id intents") && ok;
  ok = Expect(result.deduped == 1, "Helper deduped count tracks duplicates") && ok;
  ok = Expect(result.success, "Helper reports success when publish path succeeds") && ok;
  ok = Expect(records.size() == 1, "Helper forwards intents into producer path") && ok;
  if (records.size() != 1) return false;
  ok = Expect(records[0].topic == "execution.intents",
              "Helper uses execution.intents topic via producer") && ok;
  return ok;
}

bool TestHedgeTriggeredFalseDoesNotProduceIntent() {
  ExecutionIntentBuilder builder;
  std::vector<HedgeTriggerDecision> decisions;
  decisions.push_back(MakeHedgeDecision(
      "batch-hedge-1",
      "provider-1",
      "BTC/USDT",
      200,
      2,
      10000,
      2,
      false));

  const auto result = builder.BuildFromHedgeTriggerDecisions(decisions);

  bool ok = true;
  ok = Expect(result.intents.empty(), "Non-triggered hedge decision must not produce intent") && ok;
  ok = Expect(result.decisions.size() == 1, "Non-triggered hedge decision emits skip decision") && ok;
  if (result.decisions.size() == 1) {
    ok = Expect(result.decisions[0].reason == "not_triggered", "Non-triggered skip reason") && ok;
  }
  return ok;
}

bool TestHedgeBuildMapsSidesAndFieldsAndConfig() {
  ExecutionIntentBuilder builder;
  std::vector<HedgeTriggerDecision> decisions;
  decisions.push_back(MakeHedgeDecision(
      "batch-hedge-2",
      "provider-2",
      "BTC/USDT",
      250,
      2,
      10025,
      2,
      true));
  decisions.push_back(MakeHedgeDecision(
      "batch-hedge-2",
      "provider-2",
      "ETH/USDT",
      -300,
      2,
      20050,
      2,
      true));

  HedgeExecutionIntentConfig config;
  config.urgency = fob::execution::v1::URGENCY_HIGH;
  config.strategy = fob::execution::v1::EXEC_STRATEGY_LIMIT;
  config.tif = fob::common::v1::TIF_FOK;
  config.timeout_ms = 45000;
  config.max_slippage_bps = 25;
  config.allowed_venues = {"binance", "okx"};

  const auto result = builder.BuildFromHedgeTriggerDecisions(decisions, config);

  bool ok = true;
  ok = Expect(result.intents.size() == 2, "Triggered hedge decisions must produce intents") && ok;
  ok = Expect(result.decisions.size() == 2, "Triggered decisions are tracked") && ok;
  if (result.intents.size() != 2) return false;

  const auto& sell_intent = result.intents[0];
  ok = Expect(sell_intent.side() == fob::common::v1::SIDE_SELL,
              "Positive net_qty must produce SELL hedge") && ok;
  ok = ExpectNear(static_cast<double>(Decimal::from_proto(sell_intent.target_qty())), 2.5,
                  "target_qty uses abs(net_qty) for SELL case") && ok;
  ok = ExpectNear(static_cast<double>(Decimal::from_proto(sell_intent.target_notional())), 250.625,
                  "target_notional recalculated from qty * clearing_price for SELL case") && ok;
  ok = Expect(sell_intent.reference_mid().units() == 10025 &&
                  sell_intent.reference_mid().scale() == 2,
              "reference_mid maps from clearing_price") && ok;
  ok = Expect(sell_intent.batch_id() == "batch-hedge-2", "batch_id mapping for hedge intent") && ok;
  ok = Expect(sell_intent.provider_id() == "provider-2", "provider_id mapping for hedge intent") && ok;
  ok = Expect(sell_intent.instrument().symbol() == "BTC/USDT", "symbol mapping for hedge intent") && ok;
  ok = Expect(sell_intent.source() == fob::execution::v1::HEDGE_SOURCE_AUTO_BATCH,
              "source must be AUTO_BATCH") && ok;
  ok = Expect(sell_intent.reason() == "hedge", "reason must be hedge") && ok;
  ok = Expect(sell_intent.intent_id() == "batch-hedge-2|hedge|provider-2|BTC/USDT|intent",
              "intent_id must be deterministic and non-empty") && ok;
  ok = Expect(sell_intent.hedge_flow_id() == "batch-hedge-2|hedge|provider-2|BTC/USDT",
              "hedge_flow_id must be deterministic and non-empty") && ok;
  ok = Expect(sell_intent.client_order_id() == sell_intent.intent_id(),
              "client_order_id follows deterministic intent id") && ok;
  ok = Expect(sell_intent.urgency() == fob::execution::v1::URGENCY_HIGH,
              "urgency comes from config") && ok;
  ok = Expect(sell_intent.timeout_ms() == 45000, "timeout_ms comes from config") && ok;
  ok = Expect(sell_intent.strategy() == fob::execution::v1::EXEC_STRATEGY_LIMIT,
              "strategy comes from config") && ok;
  ok = Expect(sell_intent.tif() == fob::common::v1::TIF_FOK, "tif comes from config") && ok;
  ok = Expect(sell_intent.max_slippage_bps() == 25, "max_slippage_bps comes from config") && ok;
  ok = Expect(sell_intent.allowed_venues_size() == 2, "allowed venues copied from config") && ok;
  if (sell_intent.allowed_venues_size() == 2) {
    ok = Expect(sell_intent.allowed_venues(0) == "binance" &&
                    sell_intent.allowed_venues(1) == "okx",
                "allowed venues keep configured order") && ok;
  }

  const auto& buy_intent = result.intents[1];
  ok = Expect(buy_intent.side() == fob::common::v1::SIDE_BUY,
              "Negative net_qty must produce BUY hedge") && ok;
  ok = ExpectNear(static_cast<double>(Decimal::from_proto(buy_intent.target_qty())), 3.0,
                  "target_qty uses abs(net_qty) for BUY case") && ok;
  ok = ExpectNear(static_cast<double>(Decimal::from_proto(buy_intent.target_notional())), 601.5,
                  "target_notional recalculated from qty * clearing_price for BUY case") && ok;
  return ok;
}

bool TestHedgeBuildDeterministicIdentifiersAcrossRuns() {
  ExecutionIntentBuilder builder;
  std::vector<HedgeTriggerDecision> decisions;
  decisions.push_back(MakeHedgeDecision(
      "batch-hedge-3",
      "provider-3",
      "SOL/USDT",
      15,
      0,
      1250,
      2,
      true));

  const auto first = builder.BuildFromHedgeTriggerDecisions(decisions);
  const auto second = builder.BuildFromHedgeTriggerDecisions(decisions);

  bool ok = true;
  ok = Expect(first.intents.size() == 1, "First hedge build creates one intent") && ok;
  ok = Expect(second.intents.size() == 1, "Second hedge build creates one intent") && ok;
  if (first.intents.size() != 1 || second.intents.size() != 1) return false;

  ok = Expect(first.intents[0].intent_id() == second.intents[0].intent_id(),
              "Hedge intent_id is deterministic for same batch/provider/symbol") && ok;
  ok = Expect(first.intents[0].hedge_flow_id() == second.intents[0].hedge_flow_id(),
              "Hedge hedge_flow_id is deterministic for same batch/provider/symbol") && ok;
  ok = Expect(!first.intents[0].intent_id().empty() && !first.intents[0].hedge_flow_id().empty(),
              "Hedge ids are non-empty") && ok;
  return ok;
}

bool TestHedgeBuildSkipsWhenReferenceMidNonPositive() {
  ExecutionIntentBuilder builder;
  std::vector<HedgeTriggerDecision> decisions;
  decisions.push_back(MakeHedgeDecision(
      "batch-hedge-4",
      "provider-4",
      "BTC/USDT",
      1,
      0,
      0,
      2,
      true));

  const auto result = builder.BuildFromHedgeTriggerDecisions(decisions);

  bool ok = true;
  ok = Expect(result.intents.empty(), "Non-positive clearing price must skip hedge intent") && ok;
  ok = Expect(result.decisions.size() == 1, "Skip decision emitted for invalid reference mid") && ok;
  if (result.decisions.size() == 1) {
    ok = Expect(result.decisions[0].reason == "reference_mid_non_positive",
                "Skip reason for invalid reference mid") && ok;
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestMapsBuyLegToExecutionIntent() && ok;
  ok = TestMapsSellLegToExecutionIntent() && ok;
  ok = TestInfeasibleLegDoesNotProduceIntent() && ok;
  ok = TestInfeasibleOrderDoesNotProduceAnyIntent() && ok;
  ok = TestDeterministicIntentAndClientOrderId() && ok;
  ok = TestBestFeasibleComparisonVenueIsSelected() && ok;
  ok = TestBuildsIocIntentFromExternalFill() && ok;
  ok = TestPublisherWritesExecutionIntentsTopic() && ok;
  ok = TestAutoHedgePublisherUsesHedgeFlowIdAsKey() && ok;
  ok = TestAutoHedgePublisherDeduplicatesVectorByHedgeFlowId() && ok;
  ok = TestAutoHedgePublishHelperRoutesIntentsToProducer() && ok;
  ok = TestHedgeTriggeredFalseDoesNotProduceIntent() && ok;
  ok = TestHedgeBuildMapsSidesAndFieldsAndConfig() && ok;
  ok = TestHedgeBuildDeterministicIdentifiersAcrossRuns() && ok;
  ok = TestHedgeBuildSkipsWhenReferenceMidNonPositive() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] matching_execution_intents_test" << std::endl;
  return EXIT_SUCCESS;
}
