#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/planner_inputs_cache.hpp"

namespace {

cex::matching::app::VenueThresholds DefaultThresholds() {
  cex::matching::app::VenueThresholds thresholds;
  thresholds.min_health_score = 0.5;
  thresholds.min_confidence    = 0.2;
  thresholds.confidence_for_l3     = 0.8;
  thresholds.confidence_for_l2     = 0.5;
  return thresholds;
}

using cex::matching::app::ForecastSide;
using cex::matching::app::OrderForecast;
using cex::matching::app::OrderForecastRequest;
using cex::matching::app::OrderLegForecast;
using cex::matching::app::OrderLegRequest;
using cex::matching::app::PlannerInputsCache;
using cex::matching::app::PlannerVenueInput;
using cex::matching::app::VenueComparison;
using cex::matching::app::VenueComparisonRequest;

constexpr double kTolerance = 1e-6;

bool Expect(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    return false;
  }
  return true;
}

bool ExpectNear(const double actual,
                const double expected,
                const double tolerance,
                const char* message) {
  if (std::abs(actual - expected) > tolerance) {
    std::cerr << "FAILED: " << message << " expected=" << expected
              << " actual=" << actual << '\n';
    return false;
  }
  return true;
}

void FillSideCurve(
    fob::venue::v1::SideLiquidityCurve* side,
    const std::vector<double>& q_grid,
    const std::vector<double>& p_of_q,
    const std::vector<double>& s_of_q,
    const std::vector<double>& v_grid = {},
    const std::vector<double>& l_of_v_monotone = {},
    const std::vector<double>& l_of_v = {}) {
  side->clear_q_grid();
  side->clear_p_of_q();
  side->clear_s_of_q();
  side->clear_v_grid();
  side->clear_l_of_v_monotone();
  side->clear_l_of_v();

  for (const double value : q_grid) side->add_q_grid(value);
  for (const double value : p_of_q) side->add_p_of_q(value);
  for (const double value : s_of_q) side->add_s_of_q(value);
  for (const double value : v_grid) side->add_v_grid(value);
  for (const double value : l_of_v_monotone) side->add_l_of_v_monotone(value);
  for (const double value : l_of_v) side->add_l_of_v(value);
}

fob::venue::v1::VenueLiquidityCurve MakeSimpleCurve(const std::string& venue,
                                                    const std::string& symbol,
                                                    const double confidence,
                                                    const bool valid = true) {
  fob::venue::v1::VenueLiquidityCurve curve;
  curve.set_venue_id(venue);
  curve.mutable_instrument()->set_symbol(symbol);
  curve.set_confidence(confidence);
  curve.set_level("L1");                                                      
  auto* ask = curve.mutable_ask_curve();
  ask->add_q_grid(1.0);
  if (valid) {
    ask->add_p_of_q(101.0);
  }

  return curve;
}

fob::venue::v1::VenueLiquidityCurve MakeForecastCurve(
    const std::string& venue,
    const std::string& symbol,
    const double confidence = 1.0,
    const double mid_price = 100.0,
    const double tau_ms = 1000.0) {
  fob::venue::v1::VenueLiquidityCurve curve;
  curve.set_venue_id(venue);
  curve.mutable_instrument()->set_symbol(symbol);
  curve.set_confidence(confidence);
  curve.set_tau_ms(tau_ms);
  curve.set_level("L1");
  curve.mutable_mid_price()->set_units(static_cast<int64_t>(std::llround(mid_price * 1000.0)));
  curve.mutable_mid_price()->set_scale(3);

  FillSideCurve(curve.mutable_ask_curve(),
                {1.0, 2.0},
                {101.0, 102.0},
                {101.0, 204.0},
                {1.0, 2.0},
                {1.0, 1.0});
  FillSideCurve(curve.mutable_bid_curve(),
                {1.0, 2.0},
                {99.0, 98.0},
                {99.0, 196.0},
                {1.0, 2.0},
                {1.0, 1.0});
  return curve;
}

fob::venue::v1::VenueHealth MakeHealth(
    const std::string& venue,
    const double score,
    const fob::venue::v1::RoutingRecommendation routing =
        fob::venue::v1::ROUTING_RECOMMENDATION_ALLOW,
    const fob::venue::v1::VenueHealthStatus status =
        fob::venue::v1::VENUE_HEALTH_STATUS_OK) {
  fob::venue::v1::VenueHealth health;
  health.set_venue(venue);
  health.set_event_type(fob::venue::v1::VENUE_HEALTH_EVENT_TYPE_AGGREGATED);
  health.set_routing_recommendation(routing);
  health.set_status(status);
  health.set_health_score(score);
  health.set_breaker_state(fob::venue::v1::CIRCUIT_BREAKER_STATE_CLOSED);
  return health;
}

const PlannerVenueInput* FindInputByVenue(
    const std::vector<PlannerVenueInput>& inputs,
    const std::string& venue_id) {
  const auto it = std::find_if(inputs.begin(), inputs.end(),
                               [&venue_id](const PlannerVenueInput& input) {
                                 return input.venue_id == venue_id;
                               });
  if (it == inputs.end()) return nullptr;
  return &(*it);
}

const OrderLegForecast* FindLegForecastBySymbol(
    const OrderForecast& forecast,
    const std::string& symbol) {
  const auto it = std::find_if(
      forecast.leg_forecasts.begin(), forecast.leg_forecasts.end(),
      [&symbol](const OrderLegForecast& leg_forecast) {
        return leg_forecast.symbol == symbol;
      });
  if (it == forecast.leg_forecasts.end()) return nullptr;
  return &(*it);
}

bool TestStoresMultipleVenuesPerSymbol() {
  PlannerInputsCache cache(DefaultThresholds());
  cache.UpsertCurve(MakeSimpleCurve("alpha", "BTC/USDT", 0.6));
  cache.UpsertCurve(MakeSimpleCurve("beta", "BTC/USDT", 0.8));

  const auto snapshot = cache.GetPlannerInputsSnapshot();
  const auto symbol_it = snapshot.find("BTC/USDT");

  bool ok = true;
  ok = Expect(symbol_it != snapshot.end(), "snapshot contains symbol") && ok;
  if (symbol_it == snapshot.end()) return false;

  ok = Expect(symbol_it->second.size() == 2, "snapshot keeps two venues for one symbol") && ok;
  ok = Expect(FindInputByVenue(symbol_it->second, "alpha") != nullptr, "alpha input present") && ok;
  ok = Expect(FindInputByVenue(symbol_it->second, "beta") != nullptr, "beta input present") && ok;
  ok = Expect(cache.CachedCurveCount() == 2, "cache tracks two total curves") && ok;
  ok = Expect(cache.UsableCurveCount() == 2, "both curves are usable without health blocks") && ok;
  return ok;
}

bool TestHealthUpdateChangesUsableAndRejectReason() {
  PlannerInputsCache cache(DefaultThresholds());
  cache.UpsertCurve(MakeSimpleCurve("binance", "ETH/USDT", 0.9));
  cache.UpsertHealth(MakeHealth("binance", 0.9));

  auto inputs = cache.GetPlannerInputsSnapshotForSymbol("ETH/USDT");
  auto* input = FindInputByVenue(inputs, "binance");

  bool ok = true;
  ok = Expect(input != nullptr, "input exists after health upsert") && ok;
  if (input == nullptr) return false;
  ok = Expect(input->usable, "healthy venue remains usable") && ok;

  const auto update = cache.UpsertHealth(MakeHealth(
      "binance",
      0.9,
      fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK));

  ok = Expect(update.stored, "health update stored") && ok;
  ok = Expect(!update.venue_usable, "venue became unusable") && ok;
  ok = Expect(update.venue_reject_reason == "routing_block",
              "venue reject reason is routing_block") && ok;

  inputs = cache.GetPlannerInputsSnapshotForSymbol("ETH/USDT");
  input = FindInputByVenue(inputs, "binance");
  ok = Expect(input != nullptr, "input still present after block") && ok;
  if (input == nullptr) return false;
  ok = Expect(!input->usable, "blocked venue input is unusable") && ok;
  ok = Expect(input->reject_reason == "routing_block",
              "blocked venue input keeps reject reason") && ok;

  const auto legacy = cache.LegacyBestProjection();
  ok = Expect(legacy.find("ETH/USDT") == legacy.end(),
              "blocked venue removed from legacy projection") && ok;
  return ok;
}

bool TestLegacyBestSelectionIsDeterministic() {
  PlannerInputsCache cache(DefaultThresholds());
  cache.UpsertCurve(MakeSimpleCurve("alpha", "SOL/USDT", 0.9));
  cache.UpsertCurve(MakeSimpleCurve("beta", "SOL/USDT", 0.9));
  cache.UpsertCurve(MakeSimpleCurve("gamma", "SOL/USDT", 0.8));

  cache.UpsertHealth(MakeHealth("alpha", 0.7));
  cache.UpsertHealth(MakeHealth("beta", 0.8));
  cache.UpsertHealth(MakeHealth("gamma", 1.0));

  bool ok = true;
  auto projection = cache.LegacyBestProjection();
  auto projection_it = projection.find("SOL/USDT");
  ok = Expect(projection_it != projection.end(), "symbol exists in projection") && ok;
  if (projection_it == projection.end()) return false;
  ok = Expect(projection_it->second.venue_id() == "beta",
              "higher health wins when confidence is equal") && ok;

  cache.UpsertHealth(MakeHealth("beta", 0.7));
  projection = cache.LegacyBestProjection();
  projection_it = projection.find("SOL/USDT");
  ok = Expect(projection_it != projection.end(), "symbol remains in projection after tie") && ok;
  if (projection_it == projection.end()) return false;
  ok = Expect(projection_it->second.venue_id() == "alpha",
              "venue_id asc is tie-breaker after confidence and health score") && ok;
  return ok;
}

bool TestLowConfidenceCurveStillParticipatesInPlanner() {
  PlannerInputsCache cache(DefaultThresholds());
  cache.UpsertCurve(MakeForecastCurve("binance", "BTC/USDT", 0.05));
  cache.UpsertHealth(MakeHealth("binance", 0.95));

  const auto inputs = cache.GetPlannerInputsSnapshotForSymbol("BTC/USDT");
  const auto* input = FindInputByVenue(inputs, "binance");

  bool ok = true;
  ok = Expect(input != nullptr, "low-confidence venue must remain visible") && ok;
  if (input == nullptr) return false;
  ok = Expect(input->usable, "low-confidence curve must stay usable for planner") && ok;

  const auto projection = cache.LegacyBestProjection();
  const auto projection_it = projection.find("BTC/USDT");
  ok = Expect(projection_it != projection.end(),
              "low-confidence curve must still project into external liquidity") && ok;
  if (projection_it == projection.end()) return false;
  ok = Expect(projection_it->second.venue_id() == "binance",
              "projection must preserve the venue id") && ok;
  return ok;
}

bool TestInvalidCurveNotInUsableProjection() {
  PlannerInputsCache cache(DefaultThresholds());
  cache.UpsertCurve(MakeSimpleCurve("kraken", "DOGE/USDT", 0.9, false));

  const auto inputs = cache.GetPlannerInputsSnapshotForSymbol("DOGE/USDT");
  const auto* input = FindInputByVenue(inputs, "kraken");

  bool ok = true;
  ok = Expect(input != nullptr, "invalid curve is still visible in planner snapshot") && ok;
  if (input == nullptr) return false;
  ok = Expect(!input->usable, "invalid curve is marked unusable") && ok;
  ok = Expect(input->reject_reason == "empty_curve",
              "invalid curve reject reason is empty_curve") && ok;

  const auto legacy = cache.LegacyBestProjection();
  ok = Expect(legacy.find("DOGE/USDT") == legacy.end(),
              "invalid curve excluded from legacy projection") && ok;
  ok = Expect(cache.UsableCurveCount() == 0, "invalid curve does not increase usable count") && ok;
  return ok;
}

bool TestExpectedVwapUsesSOfQInterpolation() {
  PlannerInputsCache cache(DefaultThresholds());
  auto curve = MakeForecastCurve("alpha", "BTC/USDT", 1.0, 100.0);
  FillSideCurve(curve.mutable_ask_curve(),
                {1.0, 2.0},
                {100.0, 110.0},
                {100.0, 210.0},
                {1.0, 2.0},
                {1.0, 1.0});
  cache.UpsertCurve(curve);

  VenueComparisonRequest request;
  request.symbol = "BTC/USDT";
  request.side = ForecastSide::kBuy;
  request.target_qty = 1.5;
  request.target_speed = 1.0;
  request.reference_price = 100.0;

  const auto comparisons = cache.CompareVenues(request);

  bool ok = true;
  ok = Expect(comparisons.size() == 1, "one venue comparison expected") && ok;
  if (comparisons.size() != 1) return false;
  ok = Expect(comparisons[0].feasible, "comparison must be feasible") && ok;
  ok = ExpectNear(comparisons[0].expected_notional, 155.0, kTolerance,
                  "expected_notional must interpolate S(q)") && ok;
  ok = ExpectNear(comparisons[0].expected_vwap, 155.0 / 1.5, kTolerance,
                  "expected_vwap must be S(q)/q") && ok;
  return ok;
}

bool TestSideAwareBaseIsForBuyAndSell() {
  PlannerInputsCache cache(DefaultThresholds());
  auto curve = MakeForecastCurve("alpha", "ETH/USDT", 1.0, 100.0);
  FillSideCurve(curve.mutable_ask_curve(), {1.0}, {102.0}, {102.0});
  FillSideCurve(curve.mutable_bid_curve(), {1.0}, {98.0}, {98.0});
  cache.UpsertCurve(curve);

  VenueComparisonRequest buy_request;
  buy_request.symbol = "ETH/USDT";
  buy_request.side = ForecastSide::kBuy;
  buy_request.target_qty = 1.0;
  buy_request.target_speed = 0.0;
  buy_request.reference_price = 100.0;

  VenueComparisonRequest sell_request = buy_request;
  sell_request.side = ForecastSide::kSell;

  const auto buy_comparisons = cache.CompareVenues(buy_request);
  const auto sell_comparisons = cache.CompareVenues(sell_request);

  bool ok = true;
  ok = Expect(buy_comparisons.size() == 1, "buy comparison exists") && ok;
  ok = Expect(sell_comparisons.size() == 1, "sell comparison exists") && ok;
  if (buy_comparisons.size() != 1 || sell_comparisons.size() != 1) return false;

  ok = ExpectNear(buy_comparisons[0].base_is_bps, 200.0, kTolerance,
                  "BUY base IS must use (vwap/ref-1)*10000") && ok;
  ok = ExpectNear(sell_comparisons[0].base_is_bps,
                  (100.0 / 98.0 - 1.0) * 10000.0,
                  kTolerance,
                  "SELL base IS must use (ref/vwap-1)*10000") && ok;
  return ok;
}

bool TestRejectsByPriceAndSlippageLimits() {
  PlannerInputsCache cache(DefaultThresholds());
  auto curve = MakeForecastCurve("alpha", "XRP/USDT", 1.0, 100.0);
  FillSideCurve(curve.mutable_ask_curve(), {1.0}, {102.0}, {102.0});
  cache.UpsertCurve(curve);

  VenueComparisonRequest price_limit_request;
  price_limit_request.symbol = "XRP/USDT";
  price_limit_request.side = ForecastSide::kBuy;
  price_limit_request.target_qty = 1.0;
  price_limit_request.target_speed = 0.0;
  price_limit_request.reference_price = 100.0;
  price_limit_request.limit_price = 101.0;

  VenueComparisonRequest slippage_limit_request = price_limit_request;
  slippage_limit_request.limit_price = std::nullopt;
  slippage_limit_request.max_slippage_bps = 100.0;

  const auto price_limit = cache.CompareVenues(price_limit_request);
  const auto slippage_limit = cache.CompareVenues(slippage_limit_request);

  bool ok = true;
  ok = Expect(price_limit.size() == 1, "price limit comparison exists") && ok;
  ok = Expect(slippage_limit.size() == 1, "slippage limit comparison exists") && ok;
  if (price_limit.size() != 1 || slippage_limit.size() != 1) return false;

  ok = Expect(!price_limit[0].feasible, "price limit must reject") && ok;
  ok = Expect(price_limit[0].reject_reason == "price_limit", "reject reason price_limit") && ok;

  ok = Expect(!slippage_limit[0].feasible, "slippage limit must reject") && ok;
  ok = Expect(slippage_limit[0].reject_reason == "slippage_limit", "reject reason slippage_limit") && ok;
  return ok;
}

bool TestRejectsByQtyAndSpeedLimits() {
  PlannerInputsCache cache(DefaultThresholds());
  auto curve = MakeForecastCurve("alpha", "ADA/USDT", 1.0, 100.0);
  FillSideCurve(curve.mutable_ask_curve(),
                {1.0},
                {100.0},
                {100.0},
                {0.5, 1.0},
                {80.0, 100.0});
  cache.UpsertCurve(curve);

  VenueComparisonRequest qty_request;
  qty_request.symbol = "ADA/USDT";
  qty_request.side = ForecastSide::kBuy;
  qty_request.target_qty = 2.0;
  qty_request.target_speed = 0.5;
  qty_request.reference_price = 100.0;

  VenueComparisonRequest speed_request = qty_request;
  speed_request.target_qty = 1.0;
  speed_request.target_speed = 2.0;

  const auto qty_limit = cache.CompareVenues(qty_request);
  const auto speed_limit = cache.CompareVenues(speed_request);

  bool ok = true;
  ok = Expect(qty_limit.size() == 1, "qty limit comparison exists") && ok;
  ok = Expect(speed_limit.size() == 1, "speed limit comparison exists") && ok;
  if (qty_limit.size() != 1 || speed_limit.size() != 1) return false;

  ok = Expect(!qty_limit[0].feasible, "qty limit must reject") && ok;
  ok = Expect(qty_limit[0].reject_reason == "qty_limit", "reject reason qty_limit") && ok;

  ok = Expect(!speed_limit[0].feasible, "speed limit must reject") && ok;
  ok = Expect(speed_limit[0].reject_reason == "speed_limit", "reject reason speed_limit") && ok;
  return ok;
}

bool TestComparisonSortingAndPenaltyInfluence() {
  PlannerInputsCache cache(DefaultThresholds());

  auto alpha = MakeForecastCurve("alpha", "SOL/USDT", 0.8, 100.0);
  FillSideCurve(alpha.mutable_ask_curve(), {1.0}, {101.0}, {101.0});
  cache.UpsertCurve(alpha);
  cache.UpsertHealth(MakeHealth("alpha", 1.0));

  auto beta = MakeForecastCurve("beta", "SOL/USDT", 1.0, 100.0);
  FillSideCurve(beta.mutable_ask_curve(), {1.0}, {101.0}, {101.0});
  cache.UpsertCurve(beta);
  cache.UpsertHealth(MakeHealth("beta", 0.9));

  auto gamma = MakeSimpleCurve("gamma", "SOL/USDT", 1.0, false);
  cache.UpsertCurve(gamma);

  VenueComparisonRequest request;
  request.symbol = "SOL/USDT";
  request.side = ForecastSide::kBuy;
  request.target_qty = 1.0;
  request.target_speed = 0.0;
  request.reference_price = 100.0;

  const auto comparisons = cache.CompareVenues(request);

  bool ok = true;
  ok = Expect(comparisons.size() == 3, "three venues must be compared") && ok;
  if (comparisons.size() != 3) return false;

  ok = Expect(comparisons[0].venue_id == "alpha", "alpha should rank first") && ok;
  ok = Expect(comparisons[1].venue_id == "beta", "beta should rank second") && ok;
  ok = Expect(comparisons[2].venue_id == "gamma", "infeasible gamma should be last") && ok;

  ok = Expect(comparisons[0].feasible, "first venue feasible") && ok;
  ok = Expect(comparisons[1].feasible, "second venue feasible") && ok;
  ok = Expect(!comparisons[2].feasible, "third venue infeasible") && ok;

  ok = ExpectNear(comparisons[0].expected_is_bps,
                  comparisons[1].expected_is_bps,
                  kTolerance,
                  "alpha and beta expected_is must tie") && ok;
  ok = Expect(comparisons[0].health_score > comparisons[1].health_score,
              "health_score desc should break expected_is ties") && ok;
  return ok;
}

bool TestOrderForecastUsesLegWeightSignAndAggregatesLegMetrics() {
  PlannerInputsCache cache(DefaultThresholds());

  auto btc_curve = MakeForecastCurve("venue-a", "BTC/USDT", 1.0, 100.0);
  FillSideCurve(btc_curve.mutable_ask_curve(), {2.0}, {110.0}, {220.0});
  FillSideCurve(btc_curve.mutable_bid_curve(), {2.0}, {100.0}, {200.0});
  cache.UpsertCurve(btc_curve);
  cache.UpsertHealth(MakeHealth("venue-a", 1.0));

  auto eth_curve = MakeForecastCurve("venue-a", "ETH/USDT", 1.0, 100.0);
  FillSideCurve(eth_curve.mutable_ask_curve(), {1.0}, {120.0}, {120.0});
  FillSideCurve(eth_curve.mutable_bid_curve(), {1.0}, {100.0}, {100.0});
  cache.UpsertCurve(eth_curve);
  cache.UpsertHealth(MakeHealth("venue-a", 1.0));

  OrderForecastRequest request;
  request.order_id = "ord-1";
  request.target_qty = 2.0;
  request.target_speed = 1.0;
  OrderLegRequest btc_leg_request;
  btc_leg_request.symbol = "BTC/USDT";
  btc_leg_request.coefficient = 1.0;
  request.legs.push_back(btc_leg_request);

  OrderLegRequest eth_leg_request;
  eth_leg_request.symbol = "ETH/USDT";
  eth_leg_request.coefficient = -0.5;
  request.legs.push_back(eth_leg_request);
  request.reference_prices_by_symbol = {
      {"BTC/USDT", 100.0},
      {"ETH/USDT", 100.0},
  };

  const auto forecast = cache.BuildOrderForecast(request);
  const auto* btc_leg = FindLegForecastBySymbol(forecast, "BTC/USDT");
  const auto* eth_leg = FindLegForecastBySymbol(forecast, "ETH/USDT");

  bool ok = true;
  ok = Expect(forecast.feasible, "order forecast should be feasible") && ok;
  ok = Expect(forecast.leg_forecasts.size() == 2, "order forecast should contain two legs") && ok;
  ok = Expect(btc_leg != nullptr, "BTC leg forecast exists") && ok;
  ok = Expect(eth_leg != nullptr, "ETH leg forecast exists") && ok;
  if (btc_leg == nullptr || eth_leg == nullptr) return false;

  ok = Expect(btc_leg->side == ForecastSide::kBuy,
              "positive coefficient leg must be BUY") && ok;
  ok = Expect(eth_leg->side == ForecastSide::kSell,
              "negative coefficient leg must be SELL") && ok;

  ok = ExpectNear(btc_leg->requested_qty, 2.0, kTolerance,
                  "BTC leg requested_qty = abs(coef)*target_qty") && ok;
  ok = ExpectNear(eth_leg->requested_qty, 1.0, kTolerance,
                  "ETH leg requested_qty = abs(coef)*target_qty") && ok;

  ok = ExpectNear(forecast.expected_notional, 320.0, kTolerance,
                  "order expected_notional aggregates leg notionals") && ok;
  ok = ExpectNear(forecast.expected_vwap, 320.0 / 3.0, kTolerance,
                  "order expected_vwap aggregates leg notionals by qty") && ok;
  ok = ExpectNear(forecast.base_is_bps, (1000.0 * 2.0) / 3.0, kTolerance,
                  "order base_is_bps is qty-weighted from leg forecasts") && ok;
  ok = ExpectNear(forecast.expected_is_bps, forecast.base_is_bps, kTolerance,
                  "without extra penalties expected_is equals base_is") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestStoresMultipleVenuesPerSymbol() && ok;
  ok = TestHealthUpdateChangesUsableAndRejectReason() && ok;
  ok = TestLegacyBestSelectionIsDeterministic() && ok;
  ok = TestLowConfidenceCurveStillParticipatesInPlanner() && ok;
  ok = TestInvalidCurveNotInUsableProjection() && ok;
  ok = TestExpectedVwapUsesSOfQInterpolation() && ok;
  ok = TestSideAwareBaseIsForBuyAndSell() && ok;
  ok = TestRejectsByPriceAndSlippageLimits() && ok;
  ok = TestRejectsByQtyAndSpeedLimits() && ok;
  ok = TestComparisonSortingAndPenaltyInfluence() && ok;
  ok = TestOrderForecastUsesLegWeightSignAndAggregatesLegMetrics() && ok;
  return ok ? 0 : 1;
}
