#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/hedge_trigger_policy.hpp"
#include "app/run_batch_uc.hpp"
#include "app/solver_metrics.hpp"
#include "domain/solver_impl.hpp"

namespace {

using cex::matching::app::RunBatchStatus;
using cex::matching::app::RunBatchUseCase;
using cex::matching::domain::IContinuousClearingSolver;
using cex::matching::domain::ContinuousClearingSolver;

fob::common::v1::Decimal Dec(const int64_t units, const int32_t scale = 0) {
  fob::common::v1::Decimal out;
  out.set_units(units);
  out.set_scale(scale);
  return out;
}

fob::orders::v1::FlowOrder MakeOrder(const std::string& order_id,
                                     const int64_t total_qty,
                                     const int64_t remaining_qty,
                                     const int64_t max_speed) {
  fob::orders::v1::FlowOrder order;
  order.set_order_id(order_id);
  order.set_user_id("demo-user");
  order.mutable_instrument()->set_symbol("BTC/USDT");
  order.mutable_instrument()->set_base("BTC");
  order.mutable_instrument()->set_quote("USDT");
  order.set_side(fob::common::v1::SIDE_BUY);
  *order.mutable_total_qty() = Dec(total_qty);
  *order.mutable_remaining_qty() = Dec(remaining_qty);
  *order.mutable_price_low() = Dec(100);
  *order.mutable_price_high() = Dec(102);
  *order.mutable_max_speed() = Dec(max_speed);
  order.set_status(fob::common::v1::ORDER_STATUS_NEW);
  return order;
}

fob::venue::v1::VenueLiquidityCurve MakeExternalCurve(const std::string& venue_id,
                                                      const std::string& symbol) {
  fob::venue::v1::VenueLiquidityCurve curve;
  curve.set_venue_id(venue_id);
  curve.mutable_instrument()->set_symbol(symbol);
  curve.set_snapshot_id("snapshot-" + venue_id);
  curve.set_curve_id("curve-" + venue_id);
  auto* ask = curve.mutable_ask_curve();
  ask->add_q_grid(1.0);
  ask->add_p_of_q(101.0);
  return curve;
}

bool Expect(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    return false;
  }
  return true;
}

class FakeSolver final : public IContinuousClearingSolver {
 public:
  fob::matching::v1::BatchResult Solve(
      const std::vector<cex::matching::domain::FlowOrder>& active_orders,
      const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices,
      const cex::matching::domain::ExternalLiquidityBySymbol& external_liquidity) override {
    calls += 1;
    last_orders = active_orders;
    last_reference_prices = reference_prices;
    last_external_liquidity = external_liquidity;
    if (throw_on_solve) {
      throw std::runtime_error("solver failed");
    }
    if (solve_delay.count() > 0) {
      std::this_thread::sleep_for(solve_delay);
    }
    return batch_to_return;
  }

  fob::matching::v1::BatchResult batch_to_return;
  std::vector<cex::matching::domain::FlowOrder> last_orders;
  std::unordered_map<std::string, fob::common::v1::Decimal> last_reference_prices;
  cex::matching::domain::ExternalLiquidityBySymbol last_external_liquidity;
  bool throw_on_solve{false};
  std::chrono::milliseconds solve_delay{0};
  int calls{0};
};

bool TestSkipsEmptyBatch() {
  FakeSolver solver;
  bool published = false;
  RunBatchUseCase uc(
      solver,
      [&published](const auto&) {
        published = true;
        return true;
      });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  const auto result = uc.Execute("batch-empty", active_orders);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kSkippedEmpty, "empty batch skipped") && ok;
  ok = Expect(result.batch_id == "batch-empty", "empty batch keeps scheduler batch id") && ok;
  ok = Expect(!published, "empty batch not published") && ok;
  ok = Expect(solver.calls == 0, "empty batch does not call solver") && ok;
  return ok;
}

bool TestUpdatesPartialFillAfterPublish() {
  FakeSolver solver;
  auto* fill = solver.batch_to_return.add_fills();
  fill->set_order_id("order-1");
  *fill->mutable_executed_qty() = Dec(4);

  auto* update = solver.batch_to_return.add_order_updates();
  update->set_order_id("order-1");
  update->set_status(fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED);
  *update->mutable_remaining_qty() = Dec(6);
  *update->mutable_filled_qty_total() = Dec(4);

  bool published = false;
  RunBatchUseCase uc(
      solver,
      [&published](const auto&) {
        published = true;
        return true;
      });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace("order-1", cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  const auto result = uc.Execute("batch-partial", active_orders);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kExecuted, "partial batch executed") && ok;
  ok = Expect(published, "partial batch published") && ok;
  ok = Expect(active_orders.size() == 1, "partial keeps order active") && ok;
  ok = Expect(active_orders.at("order-1").remaining_qty().units == 6,
              "partial updates remaining qty") && ok;
  ok = Expect(active_orders.at("order-1").status == cex::matching::domain::FlowOrderStatus::kPartiallyFilled,
              "partial updates status") && ok;
  ok = Expect(result.fill_deltas.size() == 1, "partial returns fill delta") && ok;
  return ok;
}

bool TestRemovesFilledOrderAfterPublish() {
  FakeSolver solver;
  auto* fill = solver.batch_to_return.add_fills();
  fill->set_order_id("order-1");
  *fill->mutable_executed_qty() = Dec(10);

  auto* update = solver.batch_to_return.add_order_updates();
  update->set_order_id("order-1");
  update->set_status(fob::common::v1::ORDER_STATUS_FILLED);
  *update->mutable_remaining_qty() = Dec(0);
  *update->mutable_filled_qty_total() = Dec(10);

  RunBatchUseCase uc(
      solver,
      [](const auto&) { return true; });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace("order-1", cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  const auto result = uc.Execute("batch-filled", active_orders);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kExecuted, "filled batch executed") && ok;
  ok = Expect(active_orders.empty(), "filled order removed from active map") && ok;
  return ok;
}

bool TestPreservesStateOnPublishFailure() {
  FakeSolver solver;
  auto* fill = solver.batch_to_return.add_fills();
  fill->set_order_id("order-1");
  *fill->mutable_executed_qty() = Dec(4);

  auto* update = solver.batch_to_return.add_order_updates();
  update->set_order_id("order-1");
  update->set_status(fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED);
  *update->mutable_remaining_qty() = Dec(6);
  *update->mutable_filled_qty_total() = Dec(4);

  RunBatchUseCase uc(
      solver,
      [](const auto&) { return false; });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace("order-1", cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  const auto result = uc.Execute("batch-publish-failed", active_orders);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kFailedPublish,
              "publish failure returned") && ok;
  ok = Expect(active_orders.at("order-1").remaining_qty().units == 10,
              "publish failure keeps remaining qty") && ok;
  ok = Expect(active_orders.at("order-1").status == cex::matching::domain::FlowOrderStatus::kNew,
              "publish failure keeps status") && ok;
  ok = Expect(result.fill_deltas.empty(), "publish failure does not expose fill deltas") && ok;
  return ok;
}

bool TestReportsSolverFailure() {
  FakeSolver solver;
  solver.throw_on_solve = true;

  RunBatchUseCase uc(
      solver,
      [](const auto&) { return true; });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace("order-1", cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  const auto result = uc.Execute("batch-solver-failed", active_orders);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kFailedSolver, "solver failure returned") && ok;
  ok = Expect(active_orders.at("order-1").remaining_qty().units == 10,
              "solver failure keeps state intact") && ok;
  return ok;
}

bool TestPopulatesSolveDiagnosticsBeforePublish() {
  FakeSolver solver;
  solver.solve_delay = std::chrono::milliseconds(2);
  solver.batch_to_return.mutable_diagnostics()->set_residual_norm(0.25);
  solver.batch_to_return.mutable_diagnostics()->set_solver_diagnostics_json(
      "{\"iterations\":7}");

  fob::matching::v1::BatchResult published_batch;
  RunBatchUseCase uc(
      solver,
      [&published_batch](const auto& batch) {
        published_batch = batch;
        return true;
      });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace("order-1", cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  const auto result = uc.Execute(active_orders);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kExecuted, "diagnostics batch executed") && ok;
  ok = Expect(published_batch.diagnostics().num_active_orders() == 1,
              "num_active_orders defaulted from snapshot") && ok;
  ok = Expect(published_batch.diagnostics().residual_norm() == 0.25,
              "solver diagnostics are preserved") && ok;
  return ok;
}

bool TestSolverMetricsRenderPrometheus() {
  cex::matching::app::SolverMetrics metrics;

  fob::matching::v1::BatchResult batch;
  batch.mutable_diagnostics()->set_solve_time_ms(120);
  batch.mutable_diagnostics()->set_residual_norm(0.25);
  batch.mutable_diagnostics()->set_solver_diagnostics_json("{\"iterations\":3}");

  metrics.ObserveBatch(batch);
  metrics.ObserveError("solve_failed");

  const auto rendered = metrics.RenderPrometheus();

  bool ok = true;
  ok = Expect(rendered.find("matching_solver_solve_time_ms_bucket{le=\"250\"} 1") !=
                  std::string::npos,
              "histogram bucket is exported") && ok;
  ok = Expect(rendered.find("matching_solver_residual_norm 0.25") != std::string::npos,
              "residual norm gauge is exported") && ok;
  ok = Expect(rendered.find("matching_solver_iterations 3") != std::string::npos,
              "iterations gauge is exported") && ok;
  ok = Expect(rendered.find(
                  "matching_solver_errors_total{error_type=\"solve_failed\"} 1") !=
                  std::string::npos,
              "error counter is exported") && ok;
  return ok;
}

bool TestSimulatedSolverHandlesMultipleOrders() {
  ContinuousClearingSolver solver;
  std::vector<cex::matching::domain::FlowOrder> active_orders;
  active_orders.push_back(cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));
  auto second = MakeOrder("order-2", 8, 8, 3);
  second.set_side(fob::common::v1::SIDE_SELL);
  active_orders.push_back(cex::matching::domain::FlowOrder::from_proto(second));

  const auto batch = solver.Solve(active_orders, {}, {});

  bool ok = true;
  ok = Expect(batch.fills_size() == 2, "solver emits fills for both orders") && ok;
  ok = Expect(batch.order_updates_size() == 2, "solver emits updates for both orders") && ok;
  ok = Expect(batch.executed_rates().size() == 2, "solver emits executed rates") && ok;
  ok = Expect(batch.clear_prices().find("BTC/USDT") != batch.clear_prices().end(),
              "solver emits clear price") && ok;
  return ok;
}

bool TestPassesReferencePricesToSolver() {
  FakeSolver solver;
  solver.batch_to_return.add_order_updates()->set_order_id("order-1");
  solver.batch_to_return.mutable_order_updates(0)->set_status(
      fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED);
  *solver.batch_to_return.mutable_order_updates(0)->mutable_remaining_qty() = Dec(9);
  *solver.batch_to_return.mutable_order_updates(0)->mutable_filled_qty_total() = Dec(1);

  RunBatchUseCase uc(
      solver,
      [](const auto&) { return true; });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace("order-1", cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  std::unordered_map<std::string, fob::common::v1::Decimal> refs;
  refs["BTC/USDT"] = Dec(103);

  const auto result = uc.Execute(active_orders, refs);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kExecuted, "reference-prices batch executed") && ok;
  ok = Expect(solver.calls == 1, "solver called exactly once") && ok;
  ok = Expect(solver.last_reference_prices.size() == 1,
              "solver received one reference price") && ok;
  auto it = solver.last_reference_prices.find("BTC/USDT");
  ok = Expect(it != solver.last_reference_prices.end(),
              "solver received BTC/USDT reference price") && ok;
  if (it != solver.last_reference_prices.end()) {
    ok = Expect(it->second.units() == 103, "reference price value forwarded") && ok;
  }
  return ok;
}

bool TestPassesExternalLiquidityToSolver() {
  FakeSolver solver;

  RunBatchUseCase uc(
      solver,
      [](const auto&) { return true; });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace("order-1", cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  cex::matching::domain::ExternalLiquidityBySymbol external_liquidity;
  external_liquidity.emplace("BTC/USDT", MakeExternalCurve("binance", "BTC/USDT"));

  const auto result = uc.Execute("batch-external-liquidity", active_orders, {}, external_liquidity);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kExecuted, "external-liquidity batch executed") && ok;
  ok = Expect(solver.calls == 1, "solver called exactly once for external-liquidity batch") && ok;
  ok = Expect(solver.last_external_liquidity.size() == 1, "solver received one external curve") && ok;
  auto it = solver.last_external_liquidity.find("BTC/USDT");
  ok = Expect(it != solver.last_external_liquidity.end(),
              "solver received BTC/USDT external curve") && ok;
  if (it != solver.last_external_liquidity.end()) {
    ok = Expect(it->second.venue_id() == "binance", "external curve venue forwarded") && ok;
    ok = Expect(it->second.ask_curve().q_grid_size() == 1,
                "external curve depth forwarded") && ok;
  }
  return ok;
}

bool TestRunBatchUsesSchedulerBatchIdAndSolveTime() {
  FakeSolver solver;
  solver.solve_delay = std::chrono::milliseconds(2);

  std::string published_batch_id;
  uint32_t published_solve_time_ms = 0;
  RunBatchUseCase uc(
      solver,
      [&published_batch_id, &published_solve_time_ms](const auto& batch) {
        published_batch_id = batch.batch_id();
        published_solve_time_ms = batch.diagnostics().solve_time_ms();
        return true;
      });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace("order-1", cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  const auto result = uc.Execute("batch-from-scheduler", active_orders);

  bool ok = true;
  ok = Expect(result.batch_id == "batch-from-scheduler", "result keeps scheduler batch id") && ok;
  ok = Expect(published_batch_id == "batch-from-scheduler",
              "published batch keeps scheduler batch id") && ok;
  ok = Expect(published_solve_time_ms > 0, "published batch exposes measured solve_time_ms") && ok;
  ok = Expect(result.solve_time_ms == published_solve_time_ms,
              "result keeps published solve_time_ms") && ok;
  return ok;
}

bool TestBackfillsLiquidityAuditIntoBatchAndFills() {
  FakeSolver solver;

  auto* internal = solver.batch_to_return.add_fills();
  internal->set_order_id("order-1");
  internal->set_user_id("demo-user");
  internal->mutable_instrument()->set_symbol("BTC/USDT");
  *internal->mutable_executed_qty() = Dec(1);

  auto* external = solver.batch_to_return.add_fills();
  external->set_order_id("order-1");
  external->set_user_id("demo-user");
  external->mutable_instrument()->set_symbol("BTC/USDT");
  *external->mutable_executed_qty() = Dec(1);
  external->set_liquidity_source("cex_hedge");

  fob::matching::v1::BatchResult published_batch;
  RunBatchUseCase uc(
      solver,
      [&published_batch](const auto& batch) {
        published_batch = batch;
        return true;
      });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace(
      "order-1",
      cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  cex::matching::domain::ExternalLiquidityBySymbol external_liquidity;
  external_liquidity.emplace("BTC/USDT", MakeExternalCurve("binance", "BTC/USDT"));

  const auto result = uc.Execute("batch-audit", active_orders, {}, external_liquidity);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kExecuted, "audit batch executed") && ok;
  ok = Expect(published_batch.diagnostics().used_liquidity_size() == 1,
              "batch diagnostics include selected external curve") && ok;
  ok = Expect(result.batch.fills_size() == published_batch.fills_size(),
              "result exposes the published batch for downstream intent building") && ok;
  ok = Expect(result.batch.batch_id() == "batch-audit",
              "result batch keeps scheduler batch id") && ok;
  if (published_batch.diagnostics().used_liquidity_size() == 1) {
    const auto& used = published_batch.diagnostics().used_liquidity(0);
    ok = Expect(used.liquidity_source() == "cex_hedge", "diagnostics liquidity_source") && ok;
    ok = Expect(used.venue_id() == "binance", "diagnostics venue_id") && ok;
    ok = Expect(used.snapshot_id() == "snapshot-binance", "diagnostics snapshot_id") && ok;
    ok = Expect(used.curve_id() == "curve-binance", "diagnostics curve_id") && ok;
  }
  ok = Expect(published_batch.fills_size() == 2, "published batch keeps both fills") && ok;
  if (published_batch.fills_size() == 2) {
    const auto& published_internal = published_batch.fills(0);
    const auto& published_external = published_batch.fills(1);
    ok = Expect(published_internal.liquidity_source() == "internal",
                "internal fill default liquidity_source") && ok;
    ok = Expect(published_internal.provenance().liquidity_source() == "internal",
                "internal fill provenance liquidity_source") && ok;
    ok = Expect(published_internal.provenance().venue_id().empty(),
                "internal fill provenance must not point to venue") && ok;
    ok = Expect(published_external.provenance().liquidity_source() == "cex_hedge",
                "external fill provenance liquidity_source") && ok;
    ok = Expect(published_external.provenance().venue_id() == "binance",
                "external fill provenance venue_id") && ok;
    ok = Expect(published_external.provenance().snapshot_id() == "snapshot-binance",
                "external fill provenance snapshot_id") && ok;
    ok = Expect(published_external.provenance().curve_id() == "curve-binance",
                "external fill provenance curve_id") && ok;
  }
  return ok;
}

bool TestBatchDiagnosticsSkipUnusedExternalLiquidity() {
  FakeSolver solver;

  auto* internal = solver.batch_to_return.add_fills();
  internal->set_order_id("order-1");
  internal->set_user_id("demo-user");
  internal->mutable_instrument()->set_symbol("BTC/USDT");
  *internal->mutable_executed_qty() = Dec(1);

  fob::matching::v1::BatchResult published_batch;
  RunBatchUseCase uc(
      solver,
      [&published_batch](const auto& batch) {
        published_batch = batch;
        return true;
      });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace(
      "order-1",
      cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  cex::matching::domain::ExternalLiquidityBySymbol external_liquidity;
  external_liquidity.emplace("BTC/USDT", MakeExternalCurve("binance", "BTC/USDT"));

  const auto result = uc.Execute("batch-unused-external", active_orders, {}, external_liquidity);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kExecuted, "unused-external batch executed") && ok;
  ok = Expect(published_batch.diagnostics().used_liquidity_size() == 0,
              "batch diagnostics must not mark unused external curve as used") && ok;
  return ok;
}

bool TestRunBatchExposesPositionSnapshotsAfterPublish() {
  FakeSolver solver;

  auto* buy = solver.batch_to_return.add_fills();
  buy->set_order_id("order-1");
  buy->set_user_id("provider-1");
  buy->mutable_instrument()->set_symbol("BTC/USDT");
  buy->set_side(fob::common::v1::SIDE_BUY);
  *buy->mutable_executed_qty() = Dec(5);

  auto* sell = solver.batch_to_return.add_fills();
  sell->set_order_id("order-2");
  sell->set_user_id("provider-1");
  sell->mutable_instrument()->set_symbol("BTC/USDT");
  sell->set_side(fob::common::v1::SIDE_SELL);
  *sell->mutable_executed_qty() = Dec(3);

  (*solver.batch_to_return.mutable_clear_prices())["BTC/USDT"] = Dec(10025, 2);

  RunBatchUseCase uc(
      solver,
      [](const auto&) { return true; });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace(
      "order-1",
      cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  const auto result = uc.Execute("batch-position-snapshots", active_orders);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kExecuted, "position snapshot batch executed") && ok;
  ok = Expect(result.position_snapshots.size() == 1,
              "run_batch returns one aggregated position snapshot") && ok;
  if (result.position_snapshots.size() == 1) {
    const auto& snapshot = result.position_snapshots[0];
    ok = Expect(snapshot.provider_id == "provider-1", "snapshot keeps provider_id") && ok;
    ok = Expect(snapshot.symbol == "BTC/USDT", "snapshot keeps symbol") && ok;
    ok = Expect(snapshot.net_qty.units == 2, "snapshot net_qty reflects BUY-SELL") && ok;
    ok = Expect(snapshot.batch_id == "batch-position-snapshots", "snapshot keeps batch_id") && ok;
    ok = Expect(snapshot.clearing_price.units() == 10025, "snapshot keeps clearing_price") && ok;
    ok = Expect(snapshot.timestamp.seconds() == result.batch.timestamp().seconds(),
                "snapshot keeps batch timestamp") && ok;
  }

  return ok;
}

bool TestRunBatchExposesHedgeTriggerDecisionsAfterSnapshots() {
  FakeSolver solver;

  auto* buy = solver.batch_to_return.add_fills();
  buy->set_order_id("order-1");
  buy->set_user_id("provider-1");
  buy->mutable_instrument()->set_symbol("BTC/USDT");
  buy->set_side(fob::common::v1::SIDE_BUY);
  *buy->mutable_executed_qty() = Dec(5);

  auto* sell = solver.batch_to_return.add_fills();
  sell->set_order_id("order-2");
  sell->set_user_id("provider-1");
  sell->mutable_instrument()->set_symbol("BTC/USDT");
  sell->set_side(fob::common::v1::SIDE_SELL);
  *sell->mutable_executed_qty() = Dec(3);

  (*solver.batch_to_return.mutable_clear_prices())["BTC/USDT"] = Dec(10025, 2);

  cex::matching::app::HedgeTriggerConfig config;
  config.default_thresholds.threshold_qty = cex::common::Decimal{1, 0};
  config.default_thresholds.threshold_notional = cex::common::Decimal{1000000, 0};

  RunBatchUseCase uc(
      solver,
      [](const auto&) { return true; },
      config);

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace(
      "order-1",
      cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  const auto result = uc.Execute("batch-hedge-trigger-decisions", active_orders);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kExecuted, "hedge decisions batch executed") && ok;
  ok = Expect(result.position_snapshots.size() == 1, "hedge decisions: one position snapshot") && ok;
  ok = Expect(result.hedge_trigger_decisions.size() == 1,
              "run_batch returns one hedge trigger decision") && ok;
  if (result.hedge_trigger_decisions.size() == 1) {
    const auto& decision = result.hedge_trigger_decisions[0];
    ok = Expect(decision.snapshot.symbol == "BTC/USDT",
                "hedge decision keeps snapshot symbol") && ok;
    ok = Expect(decision.triggered, "hedge decision triggered by qty threshold") && ok;
    ok = Expect(decision.qty_threshold_exceeded, "qty threshold marked as exceeded") && ok;
    ok = Expect(!decision.notional_threshold_exceeded,
                "notional threshold is not exceeded when threshold is high") && ok;
  }
  return ok;
}

bool TestRunBatchExposesHedgeExecutionIntentsAfterTriggerDecisions() {
  FakeSolver solver;

  auto* buy = solver.batch_to_return.add_fills();
  buy->set_order_id("order-1");
  buy->set_user_id("provider-1");
  buy->mutable_instrument()->set_symbol("BTC/USDT");
  buy->set_side(fob::common::v1::SIDE_BUY);
  *buy->mutable_executed_qty() = Dec(7);

  auto* sell = solver.batch_to_return.add_fills();
  sell->set_order_id("order-2");
  sell->set_user_id("provider-1");
  sell->mutable_instrument()->set_symbol("BTC/USDT");
  sell->set_side(fob::common::v1::SIDE_SELL);
  *sell->mutable_executed_qty() = Dec(3);

  (*solver.batch_to_return.mutable_clear_prices())["BTC/USDT"] = Dec(10025, 2);

  cex::matching::app::HedgeTriggerConfig trigger_config;
  trigger_config.default_thresholds.threshold_qty = cex::common::Decimal{1, 0};

  cex::matching::app::HedgeExecutionIntentConfig intent_config;
  intent_config.urgency = fob::execution::v1::URGENCY_HIGH;
  intent_config.strategy = fob::execution::v1::EXEC_STRATEGY_LIMIT;
  intent_config.tif = fob::common::v1::TIF_FOK;
  intent_config.timeout_ms = 45000;
  intent_config.allowed_venues = {"binance", "okx"};
  intent_config.max_slippage_bps = 20;

  RunBatchUseCase uc(
      solver,
      [](const auto&) { return true; },
      trigger_config,
      intent_config);

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders.emplace(
      "order-1",
      cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));

  const auto result = uc.Execute("batch-hedge-intents", active_orders);

  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kExecuted, "hedge intents batch executed") && ok;
  ok = Expect(result.hedge_trigger_decisions.size() == 1,
              "hedge intents batch has one trigger decision") && ok;
  ok = Expect(result.hedge_execution_intents.size() == 1,
              "run_batch returns one auto-hedge execution intent") && ok;
  ok = Expect(result.hedge_execution_intent_decisions.size() == 1,
              "run_batch returns one hedge execution build decision") && ok;
  if (result.hedge_execution_intents.size() == 1) {
    const auto& intent = result.hedge_execution_intents[0];
    ok = Expect(intent.side() == fob::common::v1::SIDE_SELL,
                "positive provider net exposure maps to SELL hedge") && ok;
    ok = Expect(intent.source() == fob::execution::v1::HEDGE_SOURCE_AUTO_BATCH,
                "run_batch hedge intent source is AUTO_BATCH") && ok;
    ok = Expect(intent.batch_id() == "batch-hedge-intents",
                "run_batch hedge intent keeps scheduler batch_id") && ok;
    ok = Expect(intent.provider_id() == "provider-1",
                "run_batch hedge intent keeps provider_id") && ok;
    ok = Expect(intent.instrument().symbol() == "BTC/USDT",
                "run_batch hedge intent keeps symbol") && ok;
    ok = Expect(intent.timeout_ms() == 45000,
                "run_batch hedge intent timeout uses injected config") && ok;
    ok = Expect(intent.allowed_venues_size() == 2,
                "run_batch hedge intent copies allowed venues from config") && ok;
  }
  if (result.hedge_execution_intent_decisions.size() == 1) {
    ok = Expect(result.hedge_execution_intent_decisions[0].intent_created,
                "run_batch hedge execution build decision marks created intent") && ok;
    ok = Expect(result.hedge_execution_intent_decisions[0].reason == "built",
                "run_batch hedge execution build reason is built") && ok;
  }
  return ok;
}

bool TestSimulatedSolverAppliesUpdatedBatchInterval() {
  ContinuousClearingSolver solver;
  std::vector<cex::matching::domain::FlowOrder> active_orders;
  active_orders.push_back(cex::matching::domain::FlowOrder::from_proto(MakeOrder("order-1", 10, 10, 4)));
  auto opposite = MakeOrder("order-2", 10, 10, 4);
  opposite.set_side(fob::common::v1::SIDE_SELL);
  active_orders.push_back(cex::matching::domain::FlowOrder::from_proto(opposite));

  const auto first_batch = solver.Solve(active_orders, {});
  solver.SetSolverConfig((cex::matching::domain::SolverConfig){
    .version = 1,
    .batch_interval = std::chrono::milliseconds(500),
    .max_iterations = 1000,
    .epsilon_liquidity = 1e-9,
    .tolerance = 1e-6
  });
  const auto second_batch = solver.Solve(active_orders, {});

  auto qty_for = [](const fob::matching::v1::BatchResult& batch,
                    const std::string& order_id) {
    for (const auto& fill : batch.fills()) {
      if (fill.order_id() == order_id) {
        return (double)cex::common::Decimal::from_proto(fill.executed_qty());
      }
    }
    return 0.0;
  };

  bool ok = true;
  ok = Expect(first_batch.fills_size() == 2, "first batch contains fills") && ok;
  ok = Expect(second_batch.fills_size() == 2, "second batch contains fills") && ok;

  ok = Expect(std::abs(qty_for(first_batch, "order-1") - 2.0) < 1e-6,
              "initial batch interval affects executed qty") && ok;
  ok = Expect(std::abs(qty_for(second_batch, "order-1") - 1.0) < 1e-6,
              "updated batch interval affects executed qty") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestSkipsEmptyBatch() && ok;
  ok = TestUpdatesPartialFillAfterPublish() && ok;
  ok = TestRemovesFilledOrderAfterPublish() && ok;
  ok = TestPreservesStateOnPublishFailure() && ok;
  ok = TestReportsSolverFailure() && ok;
  ok = TestPopulatesSolveDiagnosticsBeforePublish() && ok;
  ok = TestSolverMetricsRenderPrometheus() && ok;
  ok = TestSimulatedSolverHandlesMultipleOrders() && ok;
  ok = TestPassesReferencePricesToSolver() && ok;
  ok = TestPassesExternalLiquidityToSolver() && ok;
  ok = TestRunBatchUsesSchedulerBatchIdAndSolveTime() && ok;
  ok = TestBackfillsLiquidityAuditIntoBatchAndFills() && ok;
  ok = TestBatchDiagnosticsSkipUnusedExternalLiquidity() && ok;
  ok = TestRunBatchExposesPositionSnapshotsAfterPublish() && ok;
  ok = TestRunBatchExposesHedgeTriggerDecisionsAfterSnapshots() && ok;
  ok = TestRunBatchExposesHedgeExecutionIntentsAfterTriggerDecisions() && ok;
  ok = TestSimulatedSolverAppliesUpdatedBatchInterval() && ok;
  return ok ? 0 : 1;
}
