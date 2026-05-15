#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/execution_intent_builder.hpp"
#include "app/hedge_trigger_policy.hpp"
#include "app/position_snapshot_calculator.hpp"
#include "domain/order_fill_delta.hpp"
#include "domain/solver.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "fob/orders/v1/orders.pb.h"

namespace cex::matching::app {

enum class RunBatchStatus {
  kExecuted,
  kSkippedEmpty,
  kFailedSolver,
  kFailedPublish,
};

struct RunBatchResult {
  RunBatchStatus status{RunBatchStatus::kSkippedEmpty};
  std::string batch_id;
  size_t active_before{0};
  size_t active_after{0};
  size_t fills{0};
  uint32_t solve_time_ms{0};
  std::vector<domain::OrderFillDelta> fill_deltas;
  std::vector<PositionSnapshot> position_snapshots;
  std::vector<HedgeTriggerDecision> hedge_trigger_decisions;
  std::vector<fob::execution::v1::ExecutionIntent> hedge_execution_intents;
  std::vector<HedgeExecutionIntentBuildDecision> hedge_execution_intent_decisions;
  fob::matching::v1::BatchResult batch;
};

using PublishBatchFn =
    std::function<bool(const fob::matching::v1::BatchResult&)>;

class RunBatchUseCase {
 public:
  RunBatchUseCase(domain::IContinuousClearingSolver& solver,
                  PublishBatchFn publish_batch,
                  HedgeTriggerConfig hedge_trigger_config =
                      HedgeTriggerPolicy::DefaultConfig(),
                  HedgeExecutionIntentConfig hedge_execution_intent_config =
                      HedgeExecutionIntentConfig{});

  RunBatchResult Execute(
      std::unordered_map<std::string, domain::FlowOrder>& active_orders,
      const std::unordered_map<std::string, fob::common::v1::Decimal>&
          reference_prices = {},
      const domain::ExternalLiquidityBySymbol& external_liquidity = {});

  RunBatchResult Execute(
      const std::string& batch_id,
      std::unordered_map<std::string, domain::FlowOrder>& active_orders,
      const std::unordered_map<std::string, fob::common::v1::Decimal>&
          reference_prices = {},
      const domain::ExternalLiquidityBySymbol& external_liquidity = {});

 private:
  domain::IContinuousClearingSolver& solver_;
  PublishBatchFn publish_batch_;
  PositionSnapshotCalculator position_snapshot_calculator_;
  HedgeTriggerPolicy hedge_trigger_policy_;
  ExecutionIntentBuilder execution_intent_builder_;
  HedgeExecutionIntentConfig hedge_execution_intent_config_;
};

}  // namespace cex::matching::app
