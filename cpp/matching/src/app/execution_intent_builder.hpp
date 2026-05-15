#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/hedge_trigger_policy.hpp"
#include "app/planner_inputs_cache.hpp"
#include "fob/execution/v1/execution.pb.h"
#include "fob/matching/v1/batch.pb.h"

namespace cex::matching::app {

struct ExecutionIntentBuildRequest {
  std::string batch_id;
  std::string order_id;
  OrderForecast order_forecast;
  std::optional<double> buy_limit_price;
  std::optional<double> sell_limit_price;
};

struct ExecutionIntentLegDecision {
  std::string symbol;
  ForecastSide side{ForecastSide::kBuy};
  double requested_qty{0.0};

  bool publishable{false};
  std::string selected_venue;
  bool has_limit_price{false};
  double expected_vwap{0.0};
  double expected_is_bps{0.0};
  std::string reason;
};

struct ExecutionIntentBuildResult {
  std::vector<fob::execution::v1::ExecutionIntent> intents;
  std::vector<ExecutionIntentLegDecision> leg_decisions;
};

struct HedgeExecutionIntentConfig {
  fob::execution::v1::ExecutionUrgency urgency{
      fob::execution::v1::URGENCY_MEDIUM};
  fob::execution::v1::ExecutionStrategy strategy{
      fob::execution::v1::EXEC_STRATEGY_MARKET};
  fob::common::v1::TimeInForce tif{fob::common::v1::TIF_IOC};
  int64_t timeout_ms{30000};
  int32_t max_slippage_bps{0};
  std::vector<std::string> allowed_venues;
};

struct HedgeExecutionIntentBuildDecision {
  PositionSnapshot snapshot;
  bool triggered{false};
  bool intent_created{false};
  std::string reason;
};

struct HedgeExecutionIntentBuildResult {
  std::vector<fob::execution::v1::ExecutionIntent> intents;
  std::vector<HedgeExecutionIntentBuildDecision> decisions;
};

class ExecutionIntentBuilder {
 public:
  ExecutionIntentBuildResult Build(const ExecutionIntentBuildRequest& request) const;
  ExecutionIntentBuildResult BuildFromExternalFills(
      const fob::matching::v1::BatchResult& batch) const;
  HedgeExecutionIntentBuildResult BuildFromHedgeTriggerDecisions(
      const std::vector<HedgeTriggerDecision>& decisions,
      const HedgeExecutionIntentConfig& config = HedgeExecutionIntentConfig{}) const;
};

}  // namespace cex::matching::app
