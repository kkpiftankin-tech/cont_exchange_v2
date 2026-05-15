#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/solver.hpp"
#include "fob/venue/v1/venue.pb.h"

namespace cex::matching::app {

struct PlannerVenueInput {
  std::string venue_id;
  std::string symbol;
  fob::venue::v1::VenueLiquidityCurve curve;
  std::optional<fob::venue::v1::VenueHealth> health;
  bool usable{false};
  std::string reject_reason;
};

using PlannerInputsBySymbol =
    std::unordered_map<std::string, std::vector<PlannerVenueInput>>;

struct HealthUpsertResult {
  bool stored{false};
  bool venue_usable{true};
  std::string venue;
  std::string venue_reject_reason;
  std::string ignored_reason;
  std::vector<PlannerVenueInput> affected_inputs;
};

enum class ForecastSide {
  kBuy,
  kSell,
};

const char* ForecastSideToString(ForecastSide side);

struct VenueComparisonRequest {
  std::string symbol;
  ForecastSide side{ForecastSide::kBuy};
  double target_qty{0.0};
  double target_speed{0.0};
  std::optional<double> reference_price;
  std::optional<double> limit_price;
  std::optional<double> max_slippage_bps;
};

struct VenueComparison {
  std::string venue_id;
  std::string symbol;
  ForecastSide side{ForecastSide::kBuy};

  double requested_qty{0.0};
  double requested_speed{0.0};

  double expected_notional{0.0};
  double expected_vwap{0.0};

  double base_is_bps{0.0};
  double flow_penalty_bps{0.0};
  double health_penalty_bps{0.0};
  double confidence_penalty_bps{0.0};
  double expected_is_bps{0.0};

  double health_score{1.0};
  double confidence{1.0};

  bool feasible{false};
  std::string reject_reason;
};

struct OrderLegRequest {
  std::string symbol;
  double coefficient{0.0};
};

struct OrderLegForecast {
  std::string symbol;
  double coefficient{0.0};
  ForecastSide side{ForecastSide::kBuy};
  double requested_qty{0.0};
  double requested_speed{0.0};
  std::vector<VenueComparison> comparisons;
  std::optional<VenueComparison> best_comparison;
  bool feasible{false};
  std::string reject_reason;
};

struct OrderForecastRequest {
  std::string order_id;
  double target_qty{0.0};
  double target_speed{0.0};
  std::vector<OrderLegRequest> legs;
  std::unordered_map<std::string, double> reference_prices_by_symbol;
  std::optional<double> buy_limit_price;
  std::optional<double> sell_limit_price;
  std::optional<double> max_slippage_bps;
};

struct OrderForecast {
  std::string order_id;
  double requested_qty{0.0};
  double requested_speed{0.0};
  double expected_notional{0.0};
  double expected_vwap{0.0};
  double base_is_bps{0.0};
  double flow_penalty_bps{0.0};
  double health_penalty_bps{0.0};
  double confidence_penalty_bps{0.0};
  double expected_is_bps{0.0};
  bool feasible{false};
  std::string reject_reason;
  std::vector<OrderLegForecast> leg_forecasts;
};

struct VenueThresholds {
  double min_health_score{0.5};
  double min_confidence{0.2};
  double confidence_for_l3{0.8};
  double confidence_for_l2{0.5};
};

class PlannerInputsCache {
 public:
  explicit PlannerInputsCache(VenueThresholds thresholds);

  PlannerVenueInput UpsertCurve(const fob::venue::v1::VenueLiquidityCurve& curve);
  HealthUpsertResult UpsertHealth(const fob::venue::v1::VenueHealth& health);

  PlannerInputsBySymbol GetPlannerInputsSnapshot() const;
  std::vector<PlannerVenueInput> GetPlannerInputsSnapshotForSymbol(
      const std::string& symbol) const;
  std::vector<VenueComparison> CompareVenues(
      const VenueComparisonRequest& request) const;
  OrderForecast BuildOrderForecast(const OrderForecastRequest& request) const;

  domain::ExternalLiquidityBySymbol LegacyBestProjection() const;

  std::size_t CachedCurveCount() const;
  std::size_t UsableCurveCount() const;

 private:
  struct CurveKey {
    std::string symbol;
    std::string venue_id;
    std::string level;

    bool operator==(const CurveKey& other) const {
      return symbol == other.symbol && venue_id == other.venue_id && level == other.level;
    }
  };

  struct CurveKeyHash {
    std::size_t operator()(const CurveKey& key) const {
      return std::hash<std::string>()(key.symbol) ^
             (std::hash<std::string>()(key.venue_id) << 1) ^
             (std::hash<std::string>()(key.level) << 2);
    }
  };

  struct CurveEntry {
    fob::venue::v1::VenueLiquidityCurve curve;
    bool usable{false};
    std::string reject_reason;
  };

  const CurveEntry* SelectBestCurveForVenueFallback(const std::string& symbol,
                                            const std::string& venue_id) const;
                                            
  const CurveEntry* SelectBestCurveForVenue(const std::string& symbol,
                                            const std::string& venue_id) const;

  PlannerVenueInput BuildPlannerVenueInput(const CurveKey& key,
                                           const CurveEntry& entry) const;
  void RefreshCurveEntry(const CurveKey& key, CurveEntry* entry);

  std::unordered_map<CurveKey, CurveEntry, CurveKeyHash> curves_;
  std::unordered_map<std::string, fob::venue::v1::VenueHealth> venue_health_;

  VenueThresholds thresholds_;
};

}  // namespace cex::matching::app