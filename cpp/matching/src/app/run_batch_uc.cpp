#include "app/run_batch_uc.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cex/common/decimal.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

namespace cex::matching::app {

namespace {

bool IsTerminalStatus(const fob::common::v1::OrderStatus status) {
  switch (status) {
    case fob::common::v1::ORDER_STATUS_FILLED:
    case fob::common::v1::ORDER_STATUS_CANCELED:
    case fob::common::v1::ORDER_STATUS_REJECTED:
    case fob::common::v1::ORDER_STATUS_EXPIRED:
      return true;
    default:
      return false;
  }
}

std::string NormalizeLiquiditySource(const std::string& liquidity_source) {
  return liquidity_source.empty() ? "internal" : liquidity_source;
}

std::string CurveIdForAudit(const fob::venue::v1::VenueLiquidityCurve& curve) {
  if (!curve.curve_id().empty()) {
    return curve.curve_id();
  }
  if (curve.has_meta() && !curve.meta().event_id().empty()) {
    return curve.meta().event_id();
  }
  return "";
}

std::string ProvenanceKey(const fob::matching::v1::LiquidityProvenance& provenance) {
  return provenance.liquidity_source() + "|" + provenance.venue_id() + "|" +
         provenance.snapshot_id() + "|" + provenance.curve_id();
}

void PopulateBatchLiquidityAuditFromFills(fob::matching::v1::BatchResult* batch) {
  if (batch == nullptr) {
    return;
  }

  auto* diagnostics = batch->mutable_diagnostics();
  if (!diagnostics->used_liquidity().empty()) {
    return;
  }

  std::unordered_set<std::string> seen;
  for (const auto& fill : batch->fills()) {
    const std::string liquidity_source = NormalizeLiquiditySource(fill.liquidity_source());
    if (liquidity_source == "internal") {
      continue;
    }

    fob::matching::v1::LiquidityProvenance provenance = fill.provenance();
    if (provenance.liquidity_source().empty()) {
      provenance.set_liquidity_source(liquidity_source);
    }
    if (provenance.venue_id().empty() &&
        provenance.snapshot_id().empty() &&
        provenance.curve_id().empty()) {
      continue;
    }

    const std::string key = ProvenanceKey(provenance);
    if (!seen.insert(key).second) {
      continue;
    }

    diagnostics->add_used_liquidity()->CopyFrom(provenance);
  }
}

void BackfillFillProvenance(const domain::ExternalLiquidityBySymbol& external_liquidity,
                            fob::matching::v1::FlowFill* fill) {
  if (fill == nullptr) return;

  const std::string normalized_source = NormalizeLiquiditySource(fill->liquidity_source());
  if (fill->liquidity_source().empty()) {
    fill->set_liquidity_source(normalized_source);
  }

  auto* provenance = fill->mutable_provenance();
  if (provenance->liquidity_source().empty()) {
    provenance->set_liquidity_source(normalized_source);
  }

  if (normalized_source == "internal") {
    return;
  }

  const auto it = external_liquidity.find(fill->instrument().symbol());
  if (it == external_liquidity.end()) {
    return;
  }

  if (provenance->venue_id().empty()) {
    provenance->set_venue_id(it->second.venue_id());
  }
  if (provenance->snapshot_id().empty()) {
    provenance->set_snapshot_id(it->second.snapshot_id());
  }
  if (provenance->curve_id().empty()) {
    provenance->set_curve_id(CurveIdForAudit(it->second));
  }
}

}  // namespace

RunBatchUseCase::RunBatchUseCase(domain::IContinuousClearingSolver& solver,
                                 PublishBatchFn publish_batch,
                                 HedgeTriggerConfig hedge_trigger_config,
                                 HedgeExecutionIntentConfig hedge_execution_intent_config)
    : solver_(solver),
      publish_batch_(std::move(publish_batch)),
      hedge_trigger_policy_(std::move(hedge_trigger_config)),
      hedge_execution_intent_config_(std::move(hedge_execution_intent_config)) {}

RunBatchResult RunBatchUseCase::Execute(
    std::unordered_map<std::string, domain::FlowOrder>& active_orders,
    const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices,
    const domain::ExternalLiquidityBySymbol& external_liquidity) {
  return Execute(cex::common::uuid_v4(), active_orders, reference_prices, external_liquidity);
}

RunBatchResult RunBatchUseCase::Execute(
    const std::string& batch_id,
    std::unordered_map<std::string, domain::FlowOrder>& active_orders,
    const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices,
    const domain::ExternalLiquidityBySymbol& external_liquidity) {
  RunBatchResult result;
  result.batch_id = batch_id;
  result.active_before = active_orders.size();
  result.active_after = active_orders.size();

  if (active_orders.empty()) {
    result.status = RunBatchStatus::kSkippedEmpty;
    return result;
  }

  std::vector<domain::FlowOrder> snapshot;
  snapshot.reserve(active_orders.size());
  for (const auto& [order_id, order] : active_orders) {
    (void)order_id;
    snapshot.push_back(order);
  }

  fob::matching::v1::BatchResult batch;

  const auto solve_started_at = std::chrono::steady_clock::now();
  try {
    batch = solver_.Solve(snapshot, reference_prices, external_liquidity);
  } catch (...) {
    result.status = RunBatchStatus::kFailedSolver;
    return result;
  }

  const auto solve_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - solve_started_at
  ).count();

  const int64_t max_u32 = std::numeric_limits<uint32_t>::max();
  const auto solve_time_ms = solve_elapsed_ms > max_u32
      ? std::numeric_limits<uint32_t>::max()
      : static_cast<uint32_t>(solve_elapsed_ms);
  batch.mutable_diagnostics()->set_solve_time_ms(solve_time_ms);

  batch.set_batch_id(batch_id);
  *batch.mutable_timestamp() = cex::common::now_ts();

  auto* meta = batch.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("matching");
  meta->set_correlation_id(batch.batch_id());
  meta->set_partition_key(batch.batch_id());

  if (batch.diagnostics().num_active_orders() == 0) {
    batch.mutable_diagnostics()->set_num_active_orders(
        static_cast<uint32_t>(snapshot.size()));
  }
  for (int i = 0; i < batch.fills_size(); ++i) {
    BackfillFillProvenance(external_liquidity, batch.mutable_fills(i));
  }
  PopulateBatchLiquidityAuditFromFills(&batch);
  result.solve_time_ms = batch.diagnostics().solve_time_ms();
  result.batch = batch;

  if (!publish_batch_(batch)) {
    result.status = RunBatchStatus::kFailedPublish;
    result.fills = static_cast<size_t>(batch.fills_size());
    return result;
  }
  result.position_snapshots = position_snapshot_calculator_.Calculate(batch);
  result.hedge_trigger_decisions =
      hedge_trigger_policy_.Evaluate(result.position_snapshots);
  auto hedge_intent_build_result =
      execution_intent_builder_.BuildFromHedgeTriggerDecisions(
          result.hedge_trigger_decisions,
          hedge_execution_intent_config_);
  result.hedge_execution_intents = std::move(hedge_intent_build_result.intents);
  result.hedge_execution_intent_decisions = std::move(hedge_intent_build_result.decisions);

  std::unordered_map<std::string, cex::common::Decimal> fill_by_order;
  for (const auto& fill : batch.fills()) {
    const auto qty = cex::common::Decimal::from_proto(fill.executed_qty());
    auto it = fill_by_order.find(fill.order_id());
    if (it == fill_by_order.end()) {
      fill_by_order.emplace(fill.order_id(), qty);
      continue;
    }
    it->second = cex::common::Decimal::add(it->second, qty);
  }

  result.fill_deltas.reserve(fill_by_order.size());
  for (const auto& [order_id, qty] : fill_by_order) {
    result.fill_deltas.push_back(domain::OrderFillDelta{
        .order_id = order_id,
        .executed_qty_delta = qty,
    });
  }

  for (const auto& update : batch.order_updates()) {
    auto it = active_orders.find(update.order_id());
    if (it == active_orders.end()) continue;

    if (update.has_filled_qty_total()) {
        it->second.filled_cum = cex::common::Decimal::from_proto(update.filled_qty_total());
    }

    if (update.status() != fob::common::v1::ORDER_STATUS_UNSPECIFIED) {
        switch (update.status()) {
            case fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED:
                it->second.status = domain::FlowOrderStatus::kPartiallyFilled;
                break;
            case fob::common::v1::ORDER_STATUS_FILLED:
                it->second.status = domain::FlowOrderStatus::kFilled;
                break;
            case fob::common::v1::ORDER_STATUS_CANCELED:
                it->second.status = domain::FlowOrderStatus::kCancelled;
                break;
            case fob::common::v1::ORDER_STATUS_EXPIRED:
                it->second.status = domain::FlowOrderStatus::kExpired;
                break;
            case fob::common::v1::ORDER_STATUS_REJECTED:
                it->second.status = domain::FlowOrderStatus::kLiquidated;
                break;
            default:
                break;
        }
    }

    if (update.has_updated_at()) {
        const auto& ts = update.updated_at();
        const auto d = std::chrono::seconds{ts.seconds()} +
                       std::chrono::nanoseconds{ts.nanos()};
        it->second.updated_at = std::chrono::system_clock::time_point{
            std::chrono::duration_cast<std::chrono::system_clock::duration>(d)
        };
    }

    if (IsTerminalStatus(update.status())) {
      active_orders.erase(it);
    }
  }

  for (auto it = active_orders.begin(); it != active_orders.end();) {
    const auto remaining = it->second.remaining_qty();
    const cex::common::Decimal eps{1, 6};
    if (cex::common::Decimal::cmp(remaining, eps) <= 0) {
      it->second.status = domain::FlowOrderStatus::kFilled;
      it = active_orders.erase(it);
    } else {
      ++it;
    }
  }

  result.status = RunBatchStatus::kExecuted;
  result.active_after = active_orders.size();
  result.fills = static_cast<size_t>(batch.fills_size());
  return result;
}

}  // namespace cex::matching::app
