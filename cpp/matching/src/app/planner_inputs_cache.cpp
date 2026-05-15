#include "app/planner_inputs_cache.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <ranges>

#include "app/external_venue_filter.hpp"
#include "cex/common/decimal.hpp"
#include "google/protobuf/repeated_field.h"

namespace cex::matching::app {

namespace {

constexpr double kEpsilon = 1e-9;

using DoubleField = google::protobuf::RepeatedField<double>;

double ClampUnitInterval(const double value) {
  if (!std::isfinite(value)) return 0.0;
  return std::clamp(value, 0.0, 1.0);
}

double HealthScoreOrZero(const PlannerVenueInput& input) {
  if (!input.health.has_value()) return 0.0;
  return input.health->health_score();
}

bool IsWorseLegacyCandidate(const PlannerVenueInput& lhs,
                            const PlannerVenueInput& rhs) {
  // Primary: confidence (higher is better)
  if (lhs.curve.confidence() != rhs.curve.confidence()) {
    return lhs.curve.confidence() < rhs.curve.confidence();
  }
  // Secondary: health score (higher is better)
  const double lhs_health = HealthScoreOrZero(lhs);
  const double rhs_health = HealthScoreOrZero(rhs);
  if (lhs_health != rhs_health) {
    return lhs_health < rhs_health;
  }
  // Tertiary: venue_id ascending (lower is better)
  return lhs.venue_id > rhs.venue_id;
}

bool IsValidMonotoneSeries(const DoubleField& x_values,
                           const bool non_negative) {
  if (x_values.size() == 0) return false;

  double previous = x_values[0];
  if (!std::isfinite(previous)) return false;
  if (non_negative && previous < 0.0) return false;

  for (int i = 1; i < x_values.size(); ++i) {
    const double current = x_values[i];
    if (!std::isfinite(current)) return false;
    if (non_negative && current < 0.0) return false;
    if (current < previous) return false;
    previous = current;
  }

  return true;
}

bool IsValidInterpolationGrid(const DoubleField& x_values,
                              const DoubleField& y_values,
                              const bool non_negative_x,
                              const bool non_negative_y) {
  if (x_values.size() == 0 || y_values.size() == 0 ||
      x_values.size() != y_values.size()) {
    return false;
  }

  if (!IsValidMonotoneSeries(x_values, non_negative_x)) {
    return false;
  }

  for (int i = 0; i < y_values.size(); ++i) {
    if (!std::isfinite(y_values[i])) return false;
    if (non_negative_y && y_values[i] < 0.0) return false;
  }

  return true;
}

std::optional<double> LinearInterpolate(const DoubleField& x_values,
                                        const DoubleField& y_values,
                                        const double x_query) {
  if (!IsValidInterpolationGrid(x_values, y_values, false, false)) {
    return std::nullopt;
  }

  if (!std::isfinite(x_query)) return std::nullopt;

  if (x_query <= x_values[0]) {
    return y_values[0];
  }

  const int last = x_values.size() - 1;
  if (x_query >= x_values[last]) {
    return y_values[last];
  }

  for (int i = 1; i < x_values.size(); ++i) {
    const double x_left = x_values[i - 1];
    const double x_right = x_values[i];
    if (x_query > x_right) continue;

    const double y_left = y_values[i - 1];
    const double y_right = y_values[i];
    if (x_right <= x_left) {
      return y_right;
    }

    const double t = (x_query - x_left) / (x_right - x_left);
    return y_left + t * (y_right - y_left);
  }

  return y_values[last];
}

std::optional<double> MaxValue(const DoubleField& values) {
  if (values.size() == 0) return std::nullopt;
  return values[values.size() - 1];
}

const fob::venue::v1::SideLiquidityCurve& SelectCurveBySide(
    const fob::venue::v1::VenueLiquidityCurve& curve,
    const ForecastSide side) {
  return side == ForecastSide::kBuy ? curve.ask_curve() : curve.bid_curve();
}

double MidPriceFallback(const fob::venue::v1::VenueLiquidityCurve& curve) {
  if (!curve.has_mid_price()) return 0.0;
  return static_cast<double>(cex::common::Decimal::from_proto(curve.mid_price()));
}

bool IsHigherRankedComparison(const VenueComparison& lhs,
                              const VenueComparison& rhs) {
  if (lhs.feasible != rhs.feasible) {
    return lhs.feasible && !rhs.feasible;
  }

  if (lhs.expected_is_bps != rhs.expected_is_bps) {
    return lhs.expected_is_bps < rhs.expected_is_bps;
  }

  if (lhs.health_score != rhs.health_score) {
    return lhs.health_score > rhs.health_score;
  }

  if (lhs.confidence != rhs.confidence) {
    return lhs.confidence > rhs.confidence;
  }

  return lhs.venue_id < rhs.venue_id;
}

}  // namespace

const char* ForecastSideToString(const ForecastSide side) {
  switch (side) {
    case ForecastSide::kBuy:
      return "buy";
    case ForecastSide::kSell:
      return "sell";
  }
  return "buy";
}

PlannerInputsCache::PlannerInputsCache(VenueThresholds thresholds)
    : thresholds_(thresholds) {}

PlannerVenueInput PlannerInputsCache::UpsertCurve(
    const fob::venue::v1::VenueLiquidityCurve& curve) {
  PlannerVenueInput rejected;
  rejected.curve = curve;
  rejected.symbol = curve.instrument().symbol();
  rejected.venue_id = curve.venue_id();

  if (rejected.symbol.empty() || rejected.venue_id.empty()) {
    rejected.usable = false;
    rejected.reject_reason = "missing_curve_identity";
    return rejected;
  }

  CurveKey key{rejected.symbol, rejected.venue_id, curve.level()};
  auto& entry = curves_[key];
  entry.curve = curve;
  RefreshCurveEntry(key, &entry);

  return BuildPlannerVenueInput(key, entry);
}

HealthUpsertResult PlannerInputsCache::UpsertHealth(
    const fob::venue::v1::VenueHealth& health) {
  HealthUpsertResult result;
  result.venue = health.venue();
  if (result.venue.empty()) {
    result.ignored_reason = "missing_venue";
    return result;
  }

  const auto existing = venue_health_.find(result.venue);
  if (existing != venue_health_.end() &&
      existing->second.event_type() ==
          fob::venue::v1::VENUE_HEALTH_EVENT_TYPE_AGGREGATED &&
      health.event_type() == fob::venue::v1::VENUE_HEALTH_EVENT_TYPE_RAW) {
    result.ignored_reason = "aggregated_health_preferred";
    return result;
  }

  venue_health_[result.venue] = health;
  result.stored = true;

  std::string venue_reason;
  result.venue_usable =
      VenueAllowsMatching(health, thresholds_, &venue_reason);
  if (!result.venue_usable) {
    result.venue_reject_reason = venue_reason;
  }

  for (auto& [key, entry] : curves_) {
    if (key.venue_id != result.venue) continue;
    RefreshCurveEntry(key, &entry);
    result.affected_inputs.push_back(BuildPlannerVenueInput(key, entry));
  }

  std::sort(result.affected_inputs.begin(), result.affected_inputs.end(),
            [](const PlannerVenueInput& lhs, const PlannerVenueInput& rhs) {
              if (lhs.symbol != rhs.symbol) return lhs.symbol < rhs.symbol;
              return lhs.venue_id < rhs.venue_id;
            });

  return result;
}

const PlannerInputsCache::CurveEntry* PlannerInputsCache::SelectBestCurveForVenueFallback(
  const std::string& symbol, const std::string& venue_id) const {
  const std::array<std::string, 3> levels = {"L3", "L2", "L1"};
  for (const auto& level : levels) {
    CurveKey key{symbol, venue_id, level};
    auto it = curves_.find(key);
    if (it == curves_.end()) continue;
    const auto& entry = it->second;
    if (entry.usable) return &entry;
  }

  for (const auto& level : std::views::reverse(levels)) {
    CurveKey key{symbol, venue_id, level};
    auto it = curves_.find(key);
    if (it == curves_.end()) continue;
    return &it->second;
  }
  return nullptr;
}

const PlannerInputsCache::CurveEntry* PlannerInputsCache::SelectBestCurveForVenue(
  const std::string& symbol, const std::string& venue_id) const {
  const std::array<std::string, 3> levels = {"L3", "L2", "L1"};
  for (const auto& level : levels) {
    CurveKey key{symbol, venue_id, level};
    auto it = curves_.find(key);
    if (it == curves_.end()) continue;
    const auto& entry = it->second;
    if (entry.usable) return &entry;
  }
  return nullptr;
}

PlannerInputsBySymbol PlannerInputsCache::GetPlannerInputsSnapshot() const {
  PlannerInputsBySymbol snapshot;
  for (const auto& [key, _] : curves_) {
    const CurveEntry* best = SelectBestCurveForVenueFallback(key.symbol, key.venue_id);
    if (best) {
      auto& vec = snapshot[key.symbol];
      if (std::none_of(vec.begin(), vec.end(),
                       [&](const PlannerVenueInput& input) {
                         return input.venue_id == key.venue_id;
                       })) {
        vec.push_back(BuildPlannerVenueInput(key, *best));
      }
    }
  }
  for (auto& [symbol, vec] : snapshot) {
    std::sort(vec.begin(), vec.end(),
              [](const PlannerVenueInput& lhs, const PlannerVenueInput& rhs) {
                return lhs.venue_id < rhs.venue_id;
              });
  }
  return snapshot;
}

std::vector<PlannerVenueInput> PlannerInputsCache::GetPlannerInputsSnapshotForSymbol(
    const std::string& symbol) const {
  std::vector<PlannerVenueInput> result;
  std::unordered_set<std::string> seen_venues;

  for (const auto& [key, _] : curves_) {
    if (key.symbol != symbol) continue;
    if (seen_venues.count(key.venue_id)) continue;

    const CurveEntry* best = SelectBestCurveForVenueFallback(symbol, key.venue_id);
    if (best) {
      result.push_back(BuildPlannerVenueInput(key, *best));
      seen_venues.insert(key.venue_id);
    }
  }

  std::sort(result.begin(), result.end(),
            [](const PlannerVenueInput& lhs, const PlannerVenueInput& rhs) {
              return lhs.venue_id < rhs.venue_id;
            });
  return result;
}

std::vector<VenueComparison> PlannerInputsCache::CompareVenues(
    const VenueComparisonRequest& request) const {
  std::vector<VenueComparison> comparisons;
  const auto inputs = GetPlannerInputsSnapshotForSymbol(request.symbol);
  comparisons.reserve(inputs.size());

  for (const auto& input : inputs) {
    VenueComparison comparison;
    comparison.venue_id = input.venue_id;
    comparison.symbol = request.symbol;
    comparison.side = request.side;
    comparison.requested_qty = request.target_qty;
    comparison.requested_speed = request.target_speed;
    comparison.health_score = input.health.has_value()
        ? ClampUnitInterval(input.health->health_score())
        : 1.0;
    comparison.confidence = ClampUnitInterval(input.curve.confidence());
    comparison.expected_is_bps = std::numeric_limits<double>::infinity();

    if (!input.usable) {
      comparison.reject_reason =
          input.reject_reason.empty() ? "curve_invalid" : input.reject_reason;
      comparisons.push_back(std::move(comparison));
      continue;
    }

    if (!std::isfinite(request.target_qty) || request.target_qty <= 0.0) {
      comparison.reject_reason = "qty_limit";
      comparisons.push_back(std::move(comparison));
      continue;
    }

    const auto& side_curve = SelectCurveBySide(input.curve, request.side);
    if (!IsValidInterpolationGrid(side_curve.q_grid(), side_curve.s_of_q(),
                                  true, true)) {
      comparison.reject_reason = "curve_invalid";
      comparisons.push_back(std::move(comparison));
      continue;
    }

    const auto max_qty = MaxValue(side_curve.q_grid());
    if (!max_qty.has_value() || request.target_qty > *max_qty + kEpsilon) {
      comparison.reject_reason = "qty_limit";
      comparisons.push_back(std::move(comparison));
      continue;
    }

    // S(q): linear interpolation on (q_grid, s_of_q)
    const auto maybe_notional =
        LinearInterpolate(side_curve.q_grid(), side_curve.s_of_q(),
                          request.target_qty);
    if (!maybe_notional.has_value() || !std::isfinite(*maybe_notional) ||
        *maybe_notional <= 0.0) {
      comparison.reject_reason = "curve_invalid";
      comparisons.push_back(std::move(comparison));
      continue;
    }

    comparison.expected_notional = *maybe_notional;
    comparison.expected_vwap = comparison.expected_notional /
                               std::max(request.target_qty, kEpsilon);

    double reference_price = 0.0;
    if (request.reference_price.has_value() &&
        std::isfinite(*request.reference_price) &&
        *request.reference_price > 0.0) {
      reference_price = *request.reference_price;
    } else {
      reference_price = MidPriceFallback(input.curve);
    }

    if (!(reference_price > 0.0) || !std::isfinite(reference_price)) {
      comparison.reject_reason = "reference_price_missing";
      comparisons.push_back(std::move(comparison));
      continue;
    }

    // base_is_bps:
    // BUY:  (expected_vwap / ref_price - 1) * 10000
    // SELL: (ref_price / expected_vwap - 1) * 10000
    if (request.side == ForecastSide::kBuy) {
      comparison.base_is_bps =
          (comparison.expected_vwap / reference_price - 1.0) * 10000.0;
    } else {
      comparison.base_is_bps =
          (reference_price / comparison.expected_vwap - 1.0) * 10000.0;
    }

    // L(v): interpolation by (v_grid, l_of_v_monotone), fallback to l_of_v.
    double flow_cost = 1.0;
    if (side_curve.v_grid().size() > 0) {
      const DoubleField* l_values = &side_curve.l_of_v_monotone();
      if (!IsValidInterpolationGrid(side_curve.v_grid(), *l_values, true, true)) {
        l_values = &side_curve.l_of_v();
      }
      if (!IsValidInterpolationGrid(side_curve.v_grid(), *l_values, true, true)) {
        comparison.reject_reason = "curve_invalid";
        comparisons.push_back(std::move(comparison));
        continue;
      }

      const auto max_speed = MaxValue(side_curve.v_grid());
      if (max_speed.has_value() && request.target_speed > *max_speed + kEpsilon) {
        comparison.reject_reason = "speed_limit";
        comparisons.push_back(std::move(comparison));
        continue;
      }

      const auto maybe_flow_cost = LinearInterpolate(
          side_curve.v_grid(), *l_values, std::max(request.target_speed, 0.0));
      if (!maybe_flow_cost.has_value() || !std::isfinite(*maybe_flow_cost) ||
          *maybe_flow_cost <= 0.0) {
        comparison.reject_reason = "curve_invalid";
        comparisons.push_back(std::move(comparison));
        continue;
      }

      flow_cost = *maybe_flow_cost;
    }

    const double tau_sec = input.curve.tau_ms() > 0.0
        ? input.curve.tau_ms() / 1000.0
        : 1.0;

    // flow_penalty_bps = 10000 * max(0, (L(v)*tau_sec)/(S(q)+eps) - 1)
    comparison.flow_penalty_bps = 10000.0 * std::max(
        0.0,
        (flow_cost * tau_sec) /
            (comparison.expected_notional + kEpsilon) -
            1.0);

    // health_penalty_bps = (1 - health_score) * 100
    comparison.health_penalty_bps = (1.0 - comparison.health_score) * 100.0;

    // confidence_penalty_bps = (1 - confidence) * 50
    comparison.confidence_penalty_bps =
        (1.0 - comparison.confidence) * 50.0;

    // expected_is_bps = base_is_bps + flow_penalty_bps +
    //                   health_penalty_bps + confidence_penalty_bps
    comparison.expected_is_bps = comparison.base_is_bps +
                                 comparison.flow_penalty_bps +
                                 comparison.health_penalty_bps +
                                 comparison.confidence_penalty_bps;

    if (request.limit_price.has_value() &&
        std::isfinite(*request.limit_price) &&
        *request.limit_price > 0.0) {
      if (request.side == ForecastSide::kBuy &&
          comparison.expected_vwap > *request.limit_price + kEpsilon) {
        comparison.reject_reason = "price_limit";
        comparisons.push_back(std::move(comparison));
        continue;
      }
      if (request.side == ForecastSide::kSell &&
          comparison.expected_vwap + kEpsilon < *request.limit_price) {
        comparison.reject_reason = "price_limit";
        comparisons.push_back(std::move(comparison));
        continue;
      }
    }

    if (request.max_slippage_bps.has_value() &&
        std::isfinite(*request.max_slippage_bps) &&
        *request.max_slippage_bps > 0.0 &&
        comparison.expected_is_bps > *request.max_slippage_bps + kEpsilon) {
      comparison.reject_reason = "slippage_limit";
      comparisons.push_back(std::move(comparison));
      continue;
    }

    comparison.feasible = true;
    comparisons.push_back(std::move(comparison));
  }

  std::sort(comparisons.begin(), comparisons.end(), IsHigherRankedComparison);
  return comparisons;
}

OrderForecast PlannerInputsCache::BuildOrderForecast(
    const OrderForecastRequest& request) const {
  OrderForecast forecast;
  forecast.order_id = request.order_id;
  forecast.requested_qty = request.target_qty;
  forecast.requested_speed = request.target_speed;

  if (!std::isfinite(request.target_qty) || request.target_qty <= 0.0) {
    forecast.reject_reason = "qty_limit";
    return forecast;
  }

  if (request.legs.empty()) {
    forecast.reject_reason = "no_non_zero_legs";
    return forecast;
  }

  bool has_non_zero_legs = false;
  bool all_legs_feasible = true;
  std::string first_reject_reason;

  double weighted_qty = 0.0;
  double weighted_base_is = 0.0;
  double weighted_flow_penalty = 0.0;
  double weighted_health_penalty = 0.0;
  double weighted_confidence_penalty = 0.0;
  double weighted_expected_is = 0.0;

  for (const auto& leg : request.legs) {
    if (!std::isfinite(leg.coefficient) || std::abs(leg.coefficient) <= kEpsilon) {
      continue;
    }

    has_non_zero_legs = true;

    OrderLegForecast leg_forecast;
    leg_forecast.symbol = leg.symbol;
    leg_forecast.coefficient = leg.coefficient;
    leg_forecast.side = leg.coefficient > 0.0
        ? ForecastSide::kBuy
        : ForecastSide::kSell;
    leg_forecast.requested_qty = std::abs(leg.coefficient) * request.target_qty;
    leg_forecast.requested_speed = std::abs(leg.coefficient) * request.target_speed;

    VenueComparisonRequest comparison_request;
    comparison_request.symbol = leg.symbol;
    comparison_request.side = leg_forecast.side;
    comparison_request.target_qty = leg_forecast.requested_qty;
    comparison_request.target_speed = leg_forecast.requested_speed;
    const auto reference_it = request.reference_prices_by_symbol.find(leg.symbol);
    if (reference_it != request.reference_prices_by_symbol.end()) {
      comparison_request.reference_price = reference_it->second;
    }
    comparison_request.limit_price = leg_forecast.side == ForecastSide::kBuy
        ? request.buy_limit_price
        : request.sell_limit_price;
    comparison_request.max_slippage_bps = request.max_slippage_bps;

    leg_forecast.comparisons = CompareVenues(comparison_request);

    const auto best_feasible = std::find_if(
        leg_forecast.comparisons.begin(),
        leg_forecast.comparisons.end(),
        [](const VenueComparison& comparison) {
          return comparison.feasible;
        });

    if (best_feasible != leg_forecast.comparisons.end()) {
      leg_forecast.best_comparison = *best_feasible;
      leg_forecast.feasible = true;
    } else if (!leg_forecast.comparisons.empty()) {
      leg_forecast.best_comparison = leg_forecast.comparisons.front();
      leg_forecast.reject_reason = leg_forecast.comparisons.front().reject_reason;
    } else {
      leg_forecast.reject_reason = "no_venues";
    }

    if (leg_forecast.feasible && leg_forecast.best_comparison.has_value()) {
      const auto& best = *leg_forecast.best_comparison;
      forecast.expected_notional += best.expected_notional;
      weighted_qty += leg_forecast.requested_qty;
      weighted_base_is += best.base_is_bps * leg_forecast.requested_qty;
      weighted_flow_penalty += best.flow_penalty_bps * leg_forecast.requested_qty;
      weighted_health_penalty += best.health_penalty_bps * leg_forecast.requested_qty;
      weighted_confidence_penalty +=
          best.confidence_penalty_bps * leg_forecast.requested_qty;
      weighted_expected_is += best.expected_is_bps * leg_forecast.requested_qty;
    } else {
      all_legs_feasible = false;
      if (leg_forecast.reject_reason.empty() && leg_forecast.best_comparison.has_value()) {
        leg_forecast.reject_reason = leg_forecast.best_comparison->reject_reason;
      }
      if (first_reject_reason.empty()) {
        first_reject_reason = leg.symbol + ":" +
                              (leg_forecast.reject_reason.empty()
                                   ? std::string{"infeasible"}
                                   : leg_forecast.reject_reason);
      }
    }

    forecast.leg_forecasts.push_back(std::move(leg_forecast));
  }

  if (!has_non_zero_legs) {
    forecast.reject_reason = "no_non_zero_legs";
    return forecast;
  }

  if (weighted_qty > kEpsilon) {
    forecast.expected_vwap = forecast.expected_notional / weighted_qty;
    forecast.base_is_bps = weighted_base_is / weighted_qty;
    forecast.flow_penalty_bps = weighted_flow_penalty / weighted_qty;
    forecast.health_penalty_bps = weighted_health_penalty / weighted_qty;
    forecast.confidence_penalty_bps = weighted_confidence_penalty / weighted_qty;
    forecast.expected_is_bps = weighted_expected_is / weighted_qty;
  }

  forecast.feasible = all_legs_feasible;
  if (!forecast.feasible) {
    forecast.reject_reason = first_reject_reason.empty()
        ? "leg_infeasible"
        : first_reject_reason;
  }

  return forecast;
}

domain::ExternalLiquidityBySymbol PlannerInputsCache::LegacyBestProjection() const {
  domain::ExternalLiquidityBySymbol projection;
  const auto snapshot = GetPlannerInputsSnapshot();

  for (const auto& [symbol, inputs] : snapshot) {
    std::vector<PlannerVenueInput> usable_inputs;
    std::copy_if(inputs.begin(), inputs.end(), std::back_inserter(usable_inputs),
                 [](const PlannerVenueInput& input) { return input.usable; });

    if (usable_inputs.empty()) continue;

    auto best_it = std::max_element(usable_inputs.begin(), usable_inputs.end(), IsWorseLegacyCandidate);
    projection[symbol] = best_it->curve;
  }
  return projection;
}

std::size_t PlannerInputsCache::CachedCurveCount() const {
  return curves_.size();
}

std::size_t PlannerInputsCache::UsableCurveCount() const {
  return std::count_if(curves_.begin(), curves_.end(),
                       [](const auto& kv) { return kv.second.usable; });
}

PlannerVenueInput PlannerInputsCache::BuildPlannerVenueInput(
    const CurveKey& key, const CurveEntry& entry) const {
  PlannerVenueInput input;
  input.symbol = key.symbol;
  input.venue_id = key.venue_id;
  input.curve = entry.curve;
  input.usable = entry.usable;
  input.reject_reason = entry.reject_reason;
  const auto health_it = venue_health_.find(key.venue_id);
  if (health_it != venue_health_.end()) {
    input.health = health_it->second;
  }
  return input;
}

void PlannerInputsCache::RefreshCurveEntry(const CurveKey& key, CurveEntry* entry) {
  if (entry == nullptr) return;

  const auto health_it = venue_health_.find(key.venue_id);
  const auto* health =
      health_it == venue_health_.end() ? nullptr : &health_it->second;

  std::string reason;
  entry->usable = ShouldUseVenueCurveForMatching(
      entry->curve, health, thresholds_, &reason);
  if (entry->usable) {
    entry->reject_reason.clear();
    return;
  }
  entry->reject_reason = reason;
}
}  // namespace cex::matching::app
