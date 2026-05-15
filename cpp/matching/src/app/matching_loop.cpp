#include "app/matching_loop.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/hedge_execution_intents_publisher.hpp"
#include "cex/common/decimal.hpp"
#include "cex/common/env.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "cex/common/uuid.hpp"
#include "fob/venue/v1/venue.pb.h"
#include "infra/kafka/batch_outputs_producer.hpp"
#include "infra/kafka/execution_intents_producer.hpp"

namespace cex::matching::app {

namespace {

std::unordered_map<std::string, domain::FlowOrder> MapRepoOrders(
    const std::vector<domain::FlowOrder>& repo_orders) {
  std::unordered_map<std::string, domain::FlowOrder> result;
  result.reserve(repo_orders.size());
  for (const auto& order : repo_orders) {
    result[order.order_id] = order;
  }
  return result;
}

std::vector<domain::FlowOrder> SnapshotActiveOrders(
    const std::unordered_map<std::string, domain::FlowOrder>& active_orders) {
  std::vector<domain::FlowOrder> snapshot;
  snapshot.reserve(active_orders.size());
  for (const auto& [order_id, order] : active_orders) {
    (void)order_id;
    snapshot.push_back(order);
  }
  return snapshot;
}

struct NormalizedOrderPriceLimits {
  std::optional<double> buy_limit_price;
  std::optional<double> sell_limit_price;
};

NormalizedOrderPriceLimits NormalizeOrderPriceLimits(
    const domain::FlowOrder& order) {
  const double raw_low = static_cast<double>(order.p_low);
  const double raw_high = static_cast<double>(order.p_high);
  if (!std::isfinite(raw_low) || !std::isfinite(raw_high)) {
    return {};
  }

  double normalized_low = raw_low;
  double normalized_high = raw_high;

  // Legacy SELL orders are stored as inverted-sign interval:
  // p_low = -price_high, p_high = -price_low.
  if (raw_low < 0.0 && raw_high < 0.0) {
    normalized_low = -raw_high;
    normalized_high = -raw_low;
  }

  if (normalized_low > normalized_high) {
    std::swap(normalized_low, normalized_high);
  }

  NormalizedOrderPriceLimits limits;
  if (normalized_high > 0.0) {
    limits.buy_limit_price = normalized_high;
  }
  if (normalized_low > 0.0) {
    limits.sell_limit_price = normalized_low;
  }

  return limits;
}

VenueThresholds LoadVenueHealthThresholds() {
  VenueThresholds thresholds;
  auto get_double = [](const char* name, double def) -> double {
    const auto raw = cex::common::Env::try_get_string(name);
    if (!raw.has_value()) {
      return def;
    }
    try {
      double value = std::stod(*raw);
      return std::clamp(value, 0.0, 1.0);
    } catch (...) {
      return def;
    }
  };

  thresholds.min_health_score =
      get_double("MATCHING_MIN_VENUE_HEALTH_SCORE", 0.5);
  thresholds.min_confidence = get_double("MATCHING_MIN_VENUE_CONFIDENCE", 0.2);
  thresholds.confidence_for_l3 = get_double("MATCHING_CONFIDENCE_FOR_L3", 0.8);
  thresholds.confidence_for_l2 = get_double("MATCHING_CONFIDENCE_FOR_L2", 0.5);

  return thresholds;
}

double MaxCurveQty(const fob::venue::v1::SideLiquidityCurve& curve) {
  if (curve.q_grid_size() <= 0) {
    return 0.0;
  }
  return curve.q_grid(curve.q_grid_size() - 1);
}

std::string CurveIdForLog(const fob::venue::v1::VenueLiquidityCurve& curve) {
  if (!curve.curve_id().empty()) {
    return curve.curve_id();
  }
  if (curve.has_meta() && !curve.meta().event_id().empty()) {
    return curve.meta().event_id();
  }
  return {};
}
}  // namespace

MatchingLoop::MatchingLoop(
    const std::string& brokers,
    int batch_interval_ms,
    std::shared_ptr<domain::IFlowOrderRepository> flow_order_repository,
    std::unique_ptr<domain::SolverConfigRepositoryPort> solver_config_repo,
    std::shared_ptr<infra::MarketDataClient> market_data_client,
    SolverMetrics& metrics)
    : brokers_(brokers),
      batch_interval_ms_(batch_interval_ms),
      producer_({.brokers = brokers, .client_id = "matching"}),
      consumer_({.brokers = brokers,
                 .group_id = "matching",
                 .client_id = "matching",
                 .enable_auto_commit = false}),
      metrics_(metrics),
      run_batch_uc_(solver_,
                    [this](const fob::matching::v1::BatchResult& batch) {
                      metrics_.ObserveBatch(batch);
                      return publish_batch(batch);
                    }),
      solver_config_repo_(std::move(solver_config_repo)),
      market_data_client_(std::move(market_data_client)),
      flow_order_repository_(std::move(flow_order_repository)),
      planner_inputs_cache_(LoadVenueHealthThresholds()) {}

void MatchingLoop::start() {
  running_.store(true);
  consumer_.subscribe(
      {"orders.normalized", "venue.liquidity.fob", "venue.health"});
  t_consume_ = std::thread([this] { consume_orders_loop(); });
  t_batch_ = std::thread([this] { batch_timer_loop(); });
}

void MatchingLoop::stop() {
  running_.store(false);
  if (t_consume_.joinable()) {
    t_consume_.join();
  }
  if (t_batch_.joinable()) {
    t_batch_.join();
  }
}

void MatchingLoop::consume_orders_loop() {
  while (running_.load()) {
    bool ok = consumer_.poll_once(
        500,
        [this](const std::string& topic,
               const std::string& key,
               const std::string& payload) {
          (void)key;
          if (topic == "orders.normalized") {
            fob::orders::v1::OrdersNormalized evt;
            if (!cex::common::from_bytes(payload, evt)) {
              cex::common::log_json("ERROR",
                                    "Failed to parse OrdersNormalized");
              return;
            }
            on_order_event(evt);
            return;
          }

          if (topic == "venue.liquidity.fob") {
            fob::venue::v1::VenueLiquidityCurve curve;
            if (!cex::common::from_bytes(payload, curve)) {
              cex::common::log_json("ERROR",
                                    "Failed to parse VenueLiquidityCurve");
              return;
            }
            on_liquidity_curve(curve);
            return;
          }

          if (topic == "venue.health") {
            const auto health = ParseVenueHealthMessage(payload);
            if (!health.has_value()) {
              cex::common::log_json("ERROR",
                                    "Failed to parse venue.health payload");
              return;
            }
            on_venue_health(*health);
            return;
          }
        });
    if (!ok) {
      break;
    }
  }
}

void MatchingLoop::batch_timer_loop() {
  using namespace std::chrono;
  while (running_.load()) {
    std::this_thread::sleep_for(milliseconds(refresh_batch_interval_ms()));
    if (!running_.load()) {
      break;
    }
    run_one_batch();
  }
}

int MatchingLoop::refresh_batch_interval_ms() {
  if (!solver_config_repo_) {
    return batch_interval_ms_;
  }

  try {
    const auto config = solver_config_repo_->GetActiveConfig();
    const auto configured_ms = static_cast<int>(config.batch_interval.count());
    if (configured_ms > 0) {
      batch_interval_ms_ = configured_ms;
      solver_.SetSolverConfig(config);
      cex::common::log_json(
          "INFO",
          "Updated matching batch interval from solver_config",
          {
              {"batch_interval_ms", std::to_string(batch_interval_ms_)}
      });
    }
  } catch (const std::exception& ex) {
    cex::common::log_json(
        "ERROR",
        "Failed to load active solver_config; using last interval",
        {
            {            "error",                          ex.what()},
            {"batch_interval_ms", std::to_string(batch_interval_ms_)}
    });
  }

  return batch_interval_ms_;
}

void MatchingLoop::on_order_event(
    const fob::orders::v1::OrdersNormalized& evt) {
  if (evt.has_create()) {
    const auto& o = evt.create().order();
    auto fo = domain::FlowOrder::from_proto(o);
    index_order(fo, "pending");
    active_[o.order_id()] = std::move(fo);
    cex::common::log_json(
        "INFO",
        "Order added to matching",
        {
            {"order_id",            o.order_id()},
            {  "symbol", o.instrument().symbol()}
    });
  } else if (evt.has_cancel()) {
    const auto& c = evt.cancel();
    double filled = 0.0;
    {
      auto it = active_.find(c.order_id());
      if (it != active_.end()) {
        filled = static_cast<double>(it->second.filled_cum);
      }
    }
    active_.erase(c.order_id());
    index_terminal(c.order_id(), "cancelled", filled);
    cex::common::log_json("INFO",
                          "Order canceled in matching",
                          {
                              {"order_id", c.order_id()}
    });
  } else if (evt.has_amend()) {
    const auto& a = evt.amend();
    auto it = active_.find(a.order_id());
    if (it != active_.end()) {
      // MVP: only update provided fields if non-zero.
      if (a.has_new_total_qty()) {
        it->second.q_max = cex::common::Decimal::from_proto(a.new_total_qty());
      }
      if (a.has_new_price_low()) {
        it->second.p_low = cex::common::Decimal::from_proto(a.new_price_low());
      }
      if (a.has_new_price_high()) {
        it->second.p_high =
            cex::common::Decimal::from_proto(a.new_price_high());
      }
      if (a.has_new_max_speed()) {
        it->second.q_rate = cex::common::Decimal::from_proto(a.new_max_speed());
      }
      cex::common::log_json("INFO",
                            "Order amended in matching",
                            {
                                {"order_id", a.order_id()}
      });
    }
  }
}

void MatchingLoop::on_liquidity_curve(
    const fob::venue::v1::VenueLiquidityCurve& curve) {
  PlannerVenueInput input;
  std::size_t cached_curves = 0;
  std::size_t usable_curves = 0;
  {
    std::lock_guard<std::mutex> lock(planner_inputs_cache_mutex_);
    input = planner_inputs_cache_.UpsertCurve(curve);
    cached_curves = planner_inputs_cache_.CachedCurveCount();
    usable_curves = planner_inputs_cache_.UsableCurveCount();
  }

  if (input.symbol.empty() || input.venue_id.empty()) {
    cex::common::log_json("WARN",
                          "Ignored external liquidity curve without identity",
                          {
                              {"symbol",        input.symbol},
                              { "venue",      input.venue_id},
                              {"reason", input.reject_reason}
    });
    return;
  }

  if (!input.usable) {
    cex::common::log_json(
        "WARN",
        "Rejected external liquidity curve for matching",
        {
            {       "symbol",                  input.symbol},
            {        "venue",                input.venue_id},
            {       "reason",           input.reject_reason},
            {"cached_curves", std::to_string(cached_curves)},
            {"usable_curves", std::to_string(usable_curves)}
    });
    return;
  }

  cex::common::log_json(
      "INFO",
      "Updated external liquidity curve",
      {
          {      "service",                                            "matching"},
          {    "component",                                  "execution_planning"},
          {  "participant",                                  "Execution Planning"},
          {        "stage",                              "consume_external_curve"},
          {        "topic",                                 "venue.liquidity.fob"},
          {       "symbol",                                          input.symbol},
          {        "venue",                                        input.venue_id},
          {     "curve_id",                            CurveIdForLog(input.curve)},
          {  "snapshot_id",                             input.curve.snapshot_id()},
          {        "level",                                   input.curve.level()},
          {   "confidence",              std::to_string(input.curve.confidence())},
          {   "bid_levels", std::to_string(input.curve.bid_curve().q_grid_size())},
          {   "ask_levels", std::to_string(input.curve.ask_curve().q_grid_size())},
          {  "max_bid_qty",  std::to_string(MaxCurveQty(input.curve.bid_curve()))},
          {  "max_ask_qty",  std::to_string(MaxCurveQty(input.curve.ask_curve()))},
          {"cached_curves",                         std::to_string(cached_curves)},
          {"usable_curves",                         std::to_string(usable_curves)},
          {  "source_file",              "cpp/matching/src/app/matching_loop.cpp"}
  });
}

void MatchingLoop::on_venue_health(const fob::venue::v1::VenueHealth& health) {
  HealthUpsertResult update;
  std::size_t cached_curves = 0;
  std::size_t usable_curves = 0;
  {
    std::lock_guard<std::mutex> lock(planner_inputs_cache_mutex_);
    update = planner_inputs_cache_.UpsertHealth(health);
    cached_curves = planner_inputs_cache_.CachedCurveCount();
    usable_curves = planner_inputs_cache_.UsableCurveCount();
  }

  if (update.venue.empty()) {
    return;
  }

  if (!update.stored) {
    cex::common::log_json(
        "INFO",
        "Ignored venue health update for matching",
        {
            { "venue",          update.venue},
            {"reason", update.ignored_reason}
    });
    return;
  }

  if (update.venue_usable) {
    cex::common::log_json(
        "INFO",
        "Updated venue health for matching",
        {
            {        "venue",         update.venue                             },
            {       "status", std::to_string(static_cast<int>(health.status()))},
            {      "routing",
             std::to_string(static_cast<int>(health.routing_recommendation())) },
            { "health_score",             std::to_string(health.health_score())},
            {"cached_curves",                     std::to_string(cached_curves)},
            {"usable_curves",                     std::to_string(usable_curves)}
    });
  } else {
    cex::common::log_json(
        "WARN",
        "Disabled venue for matching",
        {
            {        "venue",         update.venue                             },
            {       "reason",                        update.venue_reject_reason},
            {       "status", std::to_string(static_cast<int>(health.status()))},
            {      "routing",
             std::to_string(static_cast<int>(health.routing_recommendation())) },
            { "health_score",             std::to_string(health.health_score())},
            {"cached_curves",                     std::to_string(cached_curves)},
            {"usable_curves",                     std::to_string(usable_curves)}
    });
  }

  for (const auto& input : update.affected_inputs) {
    if (input.usable) {
      continue;
    }
    cex::common::log_json("WARN",
                          "Rejected planner venue input after health update",
                          {
                              {"symbol",        input.symbol},
                              { "venue",      input.venue_id},
                              {"reason", input.reject_reason}
    });
  }
}

domain::ExternalLiquidityBySymbol MatchingLoop::filtered_external_liquidity()
    const {
  std::lock_guard<std::mutex> lock(planner_inputs_cache_mutex_);
  return filtered_external_liquidity_unlocked();
}

domain::ExternalLiquidityBySymbol
MatchingLoop::filtered_external_liquidity_unlocked() const {
  static const bool external_disabled = []() {
    const auto raw =
        cex::common::Env::try_get_string("MATCHING_DISABLE_EXTERNAL_VENUES");
    if (!raw.has_value()) {
      return false;
    }
    const std::string& v = *raw;
    return v == "1" || v == "true" || v == "TRUE";
  }();
  if (external_disabled) {
    return {};
  }
  return planner_inputs_cache_.LegacyBestProjection();
}

void MatchingLoop::run_one_batch() {
  using namespace std::chrono;

  const auto cycle_started_at = steady_clock::now();
  const auto batch_time = std::chrono::system_clock::now();
  const auto batch_id = cex::common::uuid_v4();
  infra::ExecutionIntentsProducer execution_intents_producer(producer_);
  std::vector<fob::execution::v1::ExecutionIntent> pending_execution_intents;

  std::unordered_map<std::string, domain::FlowOrder> repo_active_orders;
  auto* active_orders = &active_;

  if (flow_order_repository_) {
    try {
      const auto loaded_orders =
          flow_order_repository_->LoadActiveFlowOrders(batch_time);
      repo_active_orders = MapRepoOrders(loaded_orders);
      active_orders = &repo_active_orders;
      cex::common::log_json(
          "INFO",
          "Loaded active flow orders from repository",
          {
              {"batch_id",                                  batch_id},
              {   "count", std::to_string(repo_active_orders.size())}
      });
    } catch (const std::exception& ex) {
      cex::common::log_json(
          "ERROR",
          "Failed to load active flow orders from repository",
          {
              {           "batch_id",batch_id                                     },
              {              "error", ex.what()},
              {"batch_cycle_time_ms",
               std::to_string(duration_cast<milliseconds>(steady_clock::now() -
               cycle_started_at)
               .count())                       }
      });
      return;
    }
  }

  std::unordered_map<std::string, fob::common::v1::Decimal> reference_prices;
  if (market_data_client_ && !active_orders->empty()) {
    cex::common::log_json(
        "INFO",
        "Batch fetching reference prices",
        {
            {"batch_id",                              batch_id},
            {  "orders", std::to_string(active_orders->size())}
    });
    try {
      reference_prices = market_data_client_->GetReferencePrices(
          SnapshotActiveOrders(*active_orders));
    } catch (const std::exception& ex) {
      cex::common::log_json("WARN",
                            "Failed to fetch reference prices for batch",
                            {
                                {"batch_id",  batch_id},
                                {   "error", ex.what()}
      });
    }
    cex::common::log_json(
        "INFO",
        "Batch reference prices done",
        {
            {"batch_id",                                batch_id},
            {     "got", std::to_string(reference_prices.size())}
    });
  }

  const double batch_interval_sec =
      static_cast<double>(batch_interval_ms_) / 1000.0;
  std::size_t intents_attempted = 0;
  std::size_t intents_published = 0;
  std::size_t hedge_execution_intents_attempted = 0;
  std::size_t hedge_execution_intents_published = 0;
  std::size_t hedge_execution_intents_deduped = 0;
  for (const auto& [order_id, order] : *active_orders) {
    (void)order_id;

    const double remaining_qty =
        std::max(0.0, static_cast<double>(order.remaining_qty()));
    const double max_speed = std::max(0.0, static_cast<double>(order.q_rate));
    const double target_qty =
        std::min(remaining_qty, max_speed * batch_interval_sec);
    if (target_qty <= 0.0 || !std::isfinite(target_qty)) {
      continue;
    }

    OrderForecastRequest forecast_request;
    forecast_request.order_id = order.order_id;
    forecast_request.target_qty = target_qty;
    forecast_request.target_speed = max_speed;
    const auto price_limits = NormalizeOrderPriceLimits(order);
    forecast_request.buy_limit_price = price_limits.buy_limit_price;
    forecast_request.sell_limit_price = price_limits.sell_limit_price;

    for (const auto& leg : order.legs) {
      const double coefficient = static_cast<double>(leg.weight);
      if (!std::isfinite(coefficient) || std::abs(coefficient) <= 1e-12) {
        continue;
      }

      OrderLegRequest leg_request;
      leg_request.symbol = leg.instrument_symbol;
      leg_request.coefficient = coefficient;
      forecast_request.legs.push_back(std::move(leg_request));

      const auto reference_it = reference_prices.find(leg.instrument_symbol);
      if (reference_it != reference_prices.end()) {
        const double reference_price = static_cast<double>(
            cex::common::Decimal::from_proto(reference_it->second));
        if (std::isfinite(reference_price) && reference_price > 0.0) {
          forecast_request.reference_prices_by_symbol[leg.instrument_symbol] =
              reference_price;
        }
      }
    }

    OrderForecast order_forecast;
    {
      std::lock_guard<std::mutex> lock(planner_inputs_cache_mutex_);
      order_forecast =
          planner_inputs_cache_.BuildOrderForecast(forecast_request);
    }
    for (const auto& leg_forecast : order_forecast.leg_forecasts) {
      const auto feasible_count = static_cast<std::size_t>(
          std::count_if(leg_forecast.comparisons.begin(),
                        leg_forecast.comparisons.end(),
                        [](const VenueComparison& comparison) {
                          return comparison.feasible;
                        }));

      std::string best_venue;
      std::string expected_vwap{"0"};
      std::string expected_is_bps{"0"};
      std::string reason = leg_forecast.reject_reason;
      if (leg_forecast.best_comparison.has_value()) {
        best_venue = leg_forecast.best_comparison->venue_id;
        expected_vwap =
            std::to_string(leg_forecast.best_comparison->expected_vwap);
        expected_is_bps =
            std::to_string(leg_forecast.best_comparison->expected_is_bps);
        if (reason.empty()) {
          reason = leg_forecast.best_comparison->reject_reason;
        }
      }

      cex::common::log_json(
          "INFO",
          "Planner venue comparisons for order leg",
          {
              {        "service",                               "matching"},
              {      "component",                     "execution_planning"},
              {    "participant",                     "Execution Planning"},
              {          "stage",                 "compare_venues_for_leg"},
              {       "order_id",                           order.order_id},
              {         "symbol",                      leg_forecast.symbol},
              {           "side",  ForecastSideToString(leg_forecast.side)},
              { "feasible_count",           std::to_string(feasible_count)},
              {     "best_venue",                               best_venue},
              {  "expected_vwap",                            expected_vwap},
              {"expected_is_bps",                          expected_is_bps},
              {       "feasible", leg_forecast.feasible ? "true" : "false"},
              {         "reason",                                   reason},
              {    "source_file", "cpp/matching/src/app/matching_loop.cpp"}
      });
    }

    if (!order_forecast.feasible) {
      cex::common::log_json("INFO",
                            "Order forecast infeasible for external planner",
                            {
                                {"batch_id",                     batch_id},
                                {"order_id",               order.order_id},
                                {  "reason", order_forecast.reject_reason}
      });
    }
  }

  std::size_t cached_curves = 0;
  std::size_t usable_curves = 0;
  domain::ExternalLiquidityBySymbol usable_external_liquidity;
  std::unordered_map<std::string, std::size_t> compared_venue_counts;
  {
    std::lock_guard<std::mutex> lock(planner_inputs_cache_mutex_);
    cached_curves = planner_inputs_cache_.CachedCurveCount();
    usable_curves = planner_inputs_cache_.UsableCurveCount();
    usable_external_liquidity = filtered_external_liquidity_unlocked();
    for (const auto& [symbol, curve] : usable_external_liquidity) {
      (void)curve;
      compared_venue_counts[symbol] =
          planner_inputs_cache_.GetPlannerInputsSnapshotForSymbol(symbol)
              .size();
    }
  }
  if (usable_curves != cached_curves) {
    cex::common::log_json(
        "INFO",
        "Filtered external venues for batch",
        {
            {        "batch_id",batch_id                                },
            {   "cached_curves", std::to_string(cached_curves)},
            {   "usable_curves", std::to_string(usable_curves)},
            {"selected_symbols",
             std::to_string(usable_external_liquidity.size()) }
    });
  }
  for (const auto& [symbol, curve] : usable_external_liquidity) {
    cex::common::log_json(
        "INFO",
        "Selected best external venue for batch symbol",
        {
            {         "batch_id",      batch_id                                 },
            {           "symbol",                                         symbol},
            {            "venue",                               curve.venue_id()},
            {      "snapshot_id",                            curve.snapshot_id()},
            {         "curve_id",                           CurveIdForLog(curve)},
            {"considered_venues",
             std::to_string(compared_venue_counts[symbol])                      },
            {      "max_bid_qty", std::to_string(MaxCurveQty(curve.bid_curve()))},
            {      "max_ask_qty", std::to_string(MaxCurveQty(curve.ask_curve()))}
    });
  }

  std::unordered_map<std::string, double> total_qty_before;
  for (const auto& [oid, o] : *active_orders) {
    total_qty_before[oid] = static_cast<double>(o.q_max);
  }

  const auto batch_result = run_batch_uc_.Execute(
      batch_id, *active_orders, reference_prices, usable_external_liquidity);
  const auto batch_cycle_time_ms =
      duration_cast<milliseconds>(steady_clock::now() - cycle_started_at)
          .count();

  {
    std::lock_guard<std::mutex> lock(order_index_mutex_);
    for (auto& [oid, snap] : order_index_) {
      auto it = active_orders->find(oid);
      if (it != active_orders->end()) {
        snap.filled_qty = static_cast<double>(it->second.filled_cum);
        if (it->second.status == domain::FlowOrderStatus::kPartiallyFilled) {
          snap.status = "partial";
        } else if (it->second.status == domain::FlowOrderStatus::kCancelled) {
          snap.status = "cancelled";
        } else if (it->second.status == domain::FlowOrderStatus::kExpired) {
          snap.status = "expired";
        }
      } else if (snap.status == "pending" || snap.status == "partial") {
        snap.filled_qty = snap.total_qty;
        snap.status = "filled";
      }
    }
  }

  if (batch_result.status == RunBatchStatus::kSkippedEmpty) {
    cex::common::log_json(
        "INFO",
        "Skipped empty batch",
        {
            {           "batch_id",               batch_result.batch_id},
            {"batch_cycle_time_ms", std::to_string(batch_cycle_time_ms)}
    });
    return;
  }

  if (batch_result.status == RunBatchStatus::kFailedSolver) {
    metrics_.ObserveError("solve_failed");
    cex::common::log_json(
        "ERROR",
        "RunBatch solver failed",
        {
            {           "batch_id",                      batch_result.batch_id},
            {      "active_before", std::to_string(batch_result.active_before)},
            {"batch_cycle_time_ms",        std::to_string(batch_cycle_time_ms)}
    });
    return;
  }

  if (batch_result.status == RunBatchStatus::kFailedPublish) {
    metrics_.ObserveError("publish_failed");
    cex::common::log_json(
        "ERROR",
        "RunBatch publish failed",
        {
            {           "batch_id",                      batch_result.batch_id},
            {              "fills",         std::to_string(batch_result.fills)},
            {      "solve_time_ms", std::to_string(batch_result.solve_time_ms)},
            {"batch_cycle_time_ms",        std::to_string(batch_cycle_time_ms)}
    });
    return;
  }

  if (flow_order_repository_ && !batch_result.fill_deltas.empty()) {
    try {
      flow_order_repository_->UpdateFilledVolumes(batch_result.fill_deltas,
                                                  batch_time);
    } catch (const std::exception& ex) {
      metrics_.ObserveError("persist_failed");
      cex::common::log_json(
          "ERROR",
          "Failed to persist flow order fills",
          {
              {   "batch_id",batch_result.batch_id                             },
              {      "error",                        ex.what()},
              {"fills_count",
               std::to_string(batch_result.fill_deltas.size())}
      });
    }
  }

  for (const auto& snapshot : batch_result.position_snapshots) {
    cex::common::log_json(
        "INFO",
        "Calculated provider position snapshot",
        {
            {    "batch_id",                                      snapshot.batch_id},
            {"provider_id",                                   snapshot.provider_id},
            {     "symbol",                                        snapshot.symbol},
            {    "net_qty",                               snapshot.net_qty.to_string()},
            {"clearing_price",
             cex::common::Decimal::from_proto(snapshot.clearing_price).to_string()}
    });
  }

  cex::common::log_json(
      "INFO",
      "Built auto-hedge execution intents from hedge trigger decisions",
      {
          {"batch_id", batch_id},
          {"hedge_trigger_decisions",
           std::to_string(batch_result.hedge_trigger_decisions.size())},
          {"hedge_execution_intents",
           std::to_string(batch_result.hedge_execution_intents.size())}
  });

  const auto hedge_publish_result = PublishAutoHedgeExecutionIntents(
      batch_id,
      batch_result.hedge_execution_intents,
      execution_intents_producer);
  hedge_execution_intents_attempted = hedge_publish_result.attempted;
  hedge_execution_intents_published = hedge_publish_result.published;
  hedge_execution_intents_deduped = hedge_publish_result.deduped;
  if (!hedge_publish_result.success) {
    metrics_.ObserveError("execution_intents_publish_failed");
    cex::common::log_json(
        "ERROR",
        "Failed to publish some auto-hedge execution intents (partial failure)",
        {
            {                        "batch_id",                             batch_id},
            {"hedge_execution_intents_attempted",
             std::to_string(hedge_execution_intents_attempted)                       },
            {"hedge_execution_intents_published",
             std::to_string(hedge_execution_intents_published)                       },
            {"hedge_execution_intents_deduped",
             std::to_string(hedge_execution_intents_deduped)}
    });
  }

  auto intent_build_result =
      execution_intent_builder_.BuildFromExternalFills(batch_result.batch);
  pending_execution_intents = std::move(intent_build_result.intents);
  intents_attempted = pending_execution_intents.size();

  for (const auto& intent : pending_execution_intents) {
    cex::common::log_json(
        "INFO",
        "Built execution intent from external fill",
        {
            {     "service",          "matching"                            },
            {   "component",                            "execution_planning"},
            { "participant",                            "Execution Planning"},
            {       "stage",                        "build_execution_intent"},
            {    "batch_id",                                        batch_id},
            {   "intent_id",                              intent.intent_id()},
            {    "order_id",                      intent.internal_order_id()},
            {      "symbol",                    intent.instrument().symbol()},
            {       "venue",                                  intent.venue()},
            {        "side", std::to_string(static_cast<int>(intent.side()))},
            {  "target_qty",
             intent.has_target_qty()
             ? cex::common::Decimal::from_proto(intent.target_qty())
             .to_string()
             : "0"                                                          },
            { "limit_price",
             intent.has_limit_price()
             ? cex::common::Decimal::from_proto(intent.limit_price())
             .to_string()
             : "0"                                                          },
            {"venue_symbol",                           intent.venue_symbol()},
            { "source_file",        "cpp/matching/src/app/matching_loop.cpp"}
    });
  }

  {
    static const bool external_disabled_intents = []() {
      const auto raw =
          cex::common::Env::try_get_string("MATCHING_DISABLE_EXTERNAL_VENUES");
      if (!raw.has_value()) {
        return false;
      }
      const std::string& v = *raw;
      return v == "1" || v == "true" || v == "TRUE";
    }();
    if (external_disabled_intents && !pending_execution_intents.empty()) {
      cex::common::log_json(
          "INFO",
          "Dropping execution intents (external venues disabled)",
          {
              {"batch_id",                                         batch_id},
              {   "count", std::to_string(pending_execution_intents.size())}
      });
      pending_execution_intents.clear();
    }
  }

  if (!pending_execution_intents.empty()) {
    if (!execution_intents_producer.produce(pending_execution_intents)) {
      metrics_.ObserveError("execution_intents_publish_failed");
      cex::common::log_json(
          "ERROR",
          "Failed to publish execution intents",
          {
              {"batch_id",                                         batch_id},
              {   "count", std::to_string(pending_execution_intents.size())}
      });
    } else {
      intents_published = pending_execution_intents.size();
    }
  }

  cex::common::log_json(
      "INFO",
      "Produced batch.outputs",
      {
          {                    "service",                                 "matching"},
          {                  "component",                         "matching_backend"},
          {                "participant",                         "Matching Backend"},
          {                      "stage",                    "publish_batch_outputs"},
          {                      "topic",                            "batch.outputs"},
          {                   "batch_id",                      batch_result.batch_id},
          {                      "fills",         std::to_string(batch_result.fills)},
          {              "active_before", std::to_string(batch_result.active_before)},
          {               "active_after",  std::to_string(batch_result.active_after)},
          {"execution_intents_attempted",          std::to_string(intents_attempted)},
          {"execution_intents_published",          std::to_string(intents_published)},
          {"hedge_execution_intents_attempted",
           std::to_string(hedge_execution_intents_attempted)},
          {"hedge_execution_intents_published",
           std::to_string(hedge_execution_intents_published)},
          {"hedge_execution_intents_deduped",
           std::to_string(hedge_execution_intents_deduped)},
          {"hedge_execution_intents",
           std::to_string(batch_result.hedge_execution_intents.size())},
          {         "position_snapshots",
           std::to_string(batch_result.position_snapshots.size())                },
          {              "solve_time_ms", std::to_string(batch_result.solve_time_ms)},
          {        "batch_cycle_time_ms",        std::to_string(batch_cycle_time_ms)},
          {                "source_file",   "cpp/matching/src/app/matching_loop.cpp"}
  });
}

bool MatchingLoop::publish_batch(const fob::matching::v1::BatchResult& batch) {
  return infra::BatchOutputsProducer(producer_).produce(batch);
  // return producer_.produce("batch.outputs", batch.batch_id(),
  // cex::common::to_bytes(batch));
}

void MatchingLoop::index_order(const domain::FlowOrder& order,
                               const std::string& status) {
  std::lock_guard<std::mutex> lock(order_index_mutex_);
  auto& snap = order_index_[order.order_id];
  snap.order_id = order.order_id;
  snap.status = status;
  snap.total_qty = static_cast<double>(order.q_max);
  snap.filled_qty = static_cast<double>(order.filled_cum);
}

void MatchingLoop::index_terminal(const std::string& order_id,
                                  const std::string& status,
                                  double filled_qty) {
  std::lock_guard<std::mutex> lock(order_index_mutex_);
  auto it = order_index_.find(order_id);
  if (it == order_index_.end()) {
    return;
  }
  it->second.status = status;
  it->second.filled_qty = filled_qty;
}

std::optional<MatchingLoop::OrderSnapshot> MatchingLoop::snapshot_order(
    const std::string& order_id) const {
  std::lock_guard<std::mutex> lock(order_index_mutex_);
  auto it = order_index_.find(order_id);
  if (it == order_index_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace cex::matching::app
