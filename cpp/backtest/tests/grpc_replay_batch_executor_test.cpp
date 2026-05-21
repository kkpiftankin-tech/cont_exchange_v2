#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <google/protobuf/empty.pb.h>

#include "cex/common/decimal.hpp"
#include "infra/grpc_replay_batch_executor.hpp"
#include "infra/in_memory_shadow_ledger.hpp"

namespace {

using cex::backtest::app::BatchOutcome;
using cex::backtest::app::CurveRow;
using cex::backtest::app::HistoricalBatch;
using cex::backtest::app::HistoricalMarketdataSnapshotRow;
using cex::backtest::app::SnapshotRow;
using cex::backtest::app::ShadowLedgerNamespaceState;
using cex::backtest::infra::GrpcReplayBatchExecutor;
using cex::backtest::infra::InMemoryShadowLedger;

fob::common::v1::Decimal Dec(double value) {
  cex::common::Decimal d{static_cast<int64_t>(std::llround(value * 100000000.0)), 8};
  return d.to_proto();
}

bool Check(bool condition, const std::string& message) {
  if (!condition) std::cerr << "FAIL: " << message << "\n";
  return condition;
}

bool Near(double actual, double expected, double eps = 1e-9) {
  return std::fabs(actual - expected) <= eps;
}

class FakeSolverStub final : public fob::matching::v1::Solver::StubInterface {
 public:
  fob::matching::v1::BatchResult response;
  fob::matching::v1::BatchRequest last_request;
  grpc::Status status{grpc::Status::OK};
  int calls{0};

  grpc::Status Solve(grpc::ClientContext*,
                     const fob::matching::v1::BatchRequest& request,
                     fob::matching::v1::BatchResult* out) override {
    ++calls;
    last_request = request;
    *out = response;
    return status;
  }

 private:
  grpc::ClientAsyncResponseReaderInterface<fob::matching::v1::BatchResult>*
  AsyncSolveRaw(grpc::ClientContext*,
                const fob::matching::v1::BatchRequest&,
                grpc::CompletionQueue*) override {
    return nullptr;
  }
  grpc::ClientAsyncResponseReaderInterface<fob::matching::v1::BatchResult>*
  PrepareAsyncSolveRaw(grpc::ClientContext*,
                       const fob::matching::v1::BatchRequest&,
                       grpc::CompletionQueue*) override {
    return nullptr;
  }
};

class FakeRiskStub final : public fob::risk::v1::RiskService::StubInterface {
 public:
  fob::risk::v1::PostTradeUpdateRequest last_post_trade;
  fob::risk::v1::PreTradeCheckRequest last_pre_trade;
  fob::risk::v1::PreTradeCheckResponse pre_trade_response;
  grpc::Status pre_trade_status{grpc::Status::OK};
  grpc::Status post_trade_status{grpc::Status::OK};
  int pre_trade_calls{0};
  int post_trade_calls{0};

  grpc::Status CheckNewOrder(grpc::ClientContext*,
                             const fob::risk::v1::PreTradeCheckRequest& request,
                             fob::risk::v1::PreTradeCheckResponse* response) override {
    ++pre_trade_calls;
    last_pre_trade = request;
    *response = pre_trade_response;
    return pre_trade_status;
  }

  grpc::Status SetKillSwitch(grpc::ClientContext*,
                             const fob::risk::v1::KillSwitchRequest&,
                             fob::risk::v1::KillSwitchResponse*) override {
    return grpc::Status::OK;
  }

  grpc::Status OnBatchResult(grpc::ClientContext*,
                             const fob::risk::v1::PostTradeUpdateRequest& request,
                             google::protobuf::Empty*) override {
    ++post_trade_calls;
    last_post_trade = request;
    return post_trade_status;
  }

 private:
  grpc::ClientAsyncResponseReaderInterface<fob::risk::v1::PreTradeCheckResponse>*
  AsyncCheckNewOrderRaw(grpc::ClientContext*,
                        const fob::risk::v1::PreTradeCheckRequest&,
                        grpc::CompletionQueue*) override {
    return nullptr;
  }
  grpc::ClientAsyncResponseReaderInterface<fob::risk::v1::PreTradeCheckResponse>*
  PrepareAsyncCheckNewOrderRaw(grpc::ClientContext*,
                               const fob::risk::v1::PreTradeCheckRequest&,
                               grpc::CompletionQueue*) override {
    return nullptr;
  }
  grpc::ClientAsyncResponseReaderInterface<fob::risk::v1::KillSwitchResponse>*
  AsyncSetKillSwitchRaw(grpc::ClientContext*,
                        const fob::risk::v1::KillSwitchRequest&,
                        grpc::CompletionQueue*) override {
    return nullptr;
  }
  grpc::ClientAsyncResponseReaderInterface<fob::risk::v1::KillSwitchResponse>*
  PrepareAsyncSetKillSwitchRaw(grpc::ClientContext*,
                               const fob::risk::v1::KillSwitchRequest&,
                               grpc::CompletionQueue*) override {
    return nullptr;
  }
  grpc::ClientAsyncResponseReaderInterface<google::protobuf::Empty>*
  AsyncOnBatchResultRaw(grpc::ClientContext*,
                        const fob::risk::v1::PostTradeUpdateRequest&,
                        grpc::CompletionQueue*) override {
    return nullptr;
  }
  grpc::ClientAsyncResponseReaderInterface<google::protobuf::Empty>*
  PrepareAsyncOnBatchResultRaw(grpc::ClientContext*,
                               const fob::risk::v1::PostTradeUpdateRequest&,
                               grpc::CompletionQueue*) override {
    return nullptr;
  }
};

class FakeReplayReader final : public cex::backtest::app::IReplayReader {
 public:
  std::vector<CurveRow> curves;
  std::string last_venue_id;
  std::string last_symbol;
  int64_t last_from_ms{0};
  int64_t last_to_ms{0};
  int load_curve_calls{0};

  std::vector<SnapshotRow> LoadSnapshots(
      const std::string&,
      const std::string&,
      int64_t,
      int64_t) override {
    return {};
  }

  std::vector<CurveRow> LoadCurves(
      const std::string& venue_id,
      const std::string& symbol,
      int64_t from_ms,
      int64_t to_ms) override {
    ++load_curve_calls;
    last_venue_id = venue_id;
    last_symbol = symbol;
    last_from_ms = from_ms;
    last_to_ms = to_ms;
    std::vector<CurveRow> rows;
    for (const auto& curve : curves) {
      if (curve.venue_id == venue_id && curve.symbol == symbol &&
          curve.event_time_ms >= from_ms && curve.event_time_ms <= to_ms) {
        rows.push_back(curve);
      }
    }
    return rows;
  }
};

HistoricalBatch MakeBatch() {
  HistoricalBatch batch;
  batch.batch_result.batch_id = "batch-1";
  batch.batch_result.event_time_ms = 1000;
  batch.batch_result.correlation_id = "corr-1";
  batch.batch_result.partition_key = "BTCUSDT";
  HistoricalMarketdataSnapshotRow md;
  md.symbol = "BTCUSDT";
  md.venue_id = "binance";
  md.event_time_ms = 1000;
  md.mid_price = 60050.0;
  md.spread = 10.0;
  batch.marketdata_snapshots.push_back(md);
  return batch;
}

HistoricalBatch MakeBatchWithHistoricalFillOnly() {
  HistoricalBatch batch;
  batch.batch_result.batch_id = "batch-2";
  batch.batch_result.event_time_ms = 1000;
  batch.batch_result.correlation_id = "corr-2";
  batch.batch_result.partition_key = "BTCUSDT";
  batch.batch_result.clear_prices_json = R"({"BTC/USDT":"60050"})";
  cex::backtest::app::HistoricalFillRow fill;
  fill.batch_id = "batch-2";
  fill.event_time_ms = 1000;
  fill.venue_id = "binance";
  fill.symbol = "BTCUSDT";
  fill.price = 60060.0;
  fill.executed_qty = 0.1;
  batch.fills.push_back(fill);
  return batch;
}

CurveRow MakeCurveRow() {
  CurveRow row;
  row.venue_id = "binance";
  row.symbol = "BTCUSDT";
  row.event_time_ms = 1000;
  row.snapshot_id = "snap-1";
  row.curve_id = "curve-1";
  row.level = "L2";
  row.ask_q_grid = "[0.1,1.0]";
  row.ask_p_of_q = "[60060,60100]";
  row.ask_s_of_q = "[6006,60100]";
  row.bid_q_grid = "[0.1,1.0]";
  row.bid_p_of_q = "[60040,60000]";
  row.bid_s_of_q = "[6004,60000]";
  row.confidence = 0.95;
  row.mid_price = 60050.0;
  row.tau_ms = 5000.0;
  return row;
}

void AddReplayFill(fob::matching::v1::BatchResult* result,
                   double residual_norm = 0.5,
                   fob::common::v1::Side side = fob::common::v1::SIDE_BUY,
                   double executed_price = 60100.0) {
  result->set_batch_id("batch-1");
  (*result->mutable_clear_prices())["BTCUSDT"] = Dec(60000.0);
  (*result->mutable_executed_rates())["replay:user-1:batch-1:1"] = Dec(0.5);
  auto* diag = result->mutable_diagnostics();
  diag->set_residual_norm(residual_norm);
  diag->set_solve_time_ms(13);

  auto* fill = result->add_fills();
  fill->set_order_id("replay:user-1:batch-1:1");
  fill->set_user_id("user-1");
  fill->mutable_instrument()->set_symbol("BTCUSDT");
  fill->mutable_instrument()->set_base("BTC");
  fill->mutable_instrument()->set_quote("USDT");
  fill->set_side(side);
  *fill->mutable_executed_qty() = Dec(2.0);
  *fill->mutable_price() = Dec(executed_price);
  *fill->mutable_executed_notional() = Dec(2.0 * executed_price);
  fill->set_liquidity_source("internal");
}

std::unique_ptr<InMemoryShadowLedger> MakeLedger() {
  auto ledger = std::make_unique<InMemoryShadowLedger>();
  ShadowLedgerNamespaceState state;
  state.namespace_id = "ns-1";
  state.session_id = "sess-1";
  state.tracked_user_id = "user-1";
  state.reporting_currency = "USDT";
  state.balances["USDT"] = "1000000.00";
  if (!ledger->CreateNamespace(state)) {
    std::cerr << "FAIL: ledger namespace setup\n";
  }
  return ledger;
}

bool TestExecutorCallsSolverRiskLedgerAndComputesMetrics() {
  auto solver = std::make_unique<FakeSolverStub>();
  auto risk = std::make_unique<FakeRiskStub>();
  auto* solver_ptr = solver.get();
  auto* risk_ptr = risk.get();
  AddReplayFill(&solver->response);
  auto ledger = MakeLedger();

  GrpcReplayBatchExecutor executor(std::move(solver), std::move(risk), ledger.get(), {});
  const auto result = executor.ExecuteBatch(
      "ns-1",
      R"({"tolerance":1.0})",
      R"([{"symbol":"BTCUSDT","side":"buy","pL":58000,"pH":62000,"qrate":1,"qmax":4,"executionwindow":3600}])",
      "user-1",
      "USDT",
      MakeBatch());

  bool ok = true;
  ok &= Check(result.outcome == BatchOutcome::kOk, "batch should succeed");
  ok &= Check(solver_ptr->calls == 1, "solver called once");
  ok &= Check(risk_ptr->pre_trade_calls == 1, "risk pre-trade called once");
  ok &= Check(risk_ptr->post_trade_calls == 1, "risk post-trade called once");
  ok &= Check(risk_ptr->last_pre_trade.order().order_id() ==
                  "replay:user-1:batch-1:1",
              "risk pre-trade saw replay order before solver");
  ok &= Check(solver_ptr->last_request.flow_orders_size() == 1, "strategy sent to solver");
  ok &= Check(solver_ptr->last_request.reference_prices().contains("BTCUSDT"),
              "marketdata midpoint sent as solver reference price");
  ok &= Check(Near(static_cast<double>(cex::common::Decimal::from_proto(
                   solver_ptr->last_request.reference_prices().at("BTCUSDT"))),
                   60050.0),
              "solver reference price value");
  ok &= Check(solver_ptr->last_request.solver_config_snapshot_json() ==
                  R"({"tolerance":1.0})",
              "solver config snapshot sent to matching isolation");
  ok &= Check(risk_ptr->last_post_trade.batch().fills_size() == 1, "risk saw solver fills");
  ok &= Check(result.fills_applied == 1, "fills_applied");
  ok &= Check(Near(result.executed_qty, 2.0), "executed_qty");
  ok &= Check(Near(result.requested_qty, 4.0), "requested_qty");
  ok &= Check(Near(result.fill_rate, 50.0), "fillrate uses execqty / qmax * 100");
  ok &= Check(Near(result.is_value, 50.0), "IS buy = execPrice - mid");
  ok &= Check(Near(result.vwap, 60100.0), "VWAP");
  ok &= Check(result.state_json.find("\"marketdata\"") != std::string::npos,
              "state_json persisted");
  ok &= Check(result.action_json.find("\"qmax\":4") != std::string::npos,
              "action_json persisted");
  ok &= Check(result.fills_json.find("\"execprice\":60100") != std::string::npos,
              "fills_json persisted");
  return ok;
}

bool TestExecutorSendsHistoricalExternalLiquidityToSolver() {
  auto solver = std::make_unique<FakeSolverStub>();
  auto risk = std::make_unique<FakeRiskStub>();
  auto* solver_ptr = solver.get();
  AddReplayFill(&solver->response);
  auto ledger = MakeLedger();
  FakeReplayReader replay_reader;
  replay_reader.curves.push_back(MakeCurveRow());

  GrpcReplayBatchExecutor executor(
      std::move(solver), std::move(risk), ledger.get(), {}, &replay_reader);
  const auto result = executor.ExecuteBatch(
      "ns-1",
      R"({"tolerance":1.0})",
      R"([{"symbol":"BTCUSDT","side":"buy","pL":58000,"pH":62000,"qrate":1,"qmax":4,"executionwindow":3600}])",
      "user-1",
      "USDT",
      MakeBatch());

  bool ok = true;
  ok &= Check(result.outcome == BatchOutcome::kOk, "batch should succeed");
  ok &= Check(replay_reader.load_curve_calls >= 1,
              "historical curves loaded for replay batch");
  ok &= Check(solver_ptr->last_request.external_liquidity_size() == 1,
              "solver request carries one historical external curve");
  if (solver_ptr->last_request.external_liquidity_size() == 1) {
    const auto& curve = solver_ptr->last_request.external_liquidity(0);
    ok &= Check(curve.venue_id() == "binance", "external curve venue propagated");
    ok &= Check(curve.instrument().symbol() == "BTCUSDT",
                "external curve symbol propagated");
    ok &= Check(curve.ask_curve().q_grid_size() == 2,
                "external ask quantity grid propagated");
  }
  return ok;
}

bool TestExecutorUsesHistoricalFillFallbackWhenSnapshotsAreAbsent() {
  auto solver = std::make_unique<FakeSolverStub>();
  auto risk = std::make_unique<FakeRiskStub>();
  auto* solver_ptr = solver.get();
  AddReplayFill(&solver->response);
  auto ledger = MakeLedger();
  FakeReplayReader replay_reader;
  replay_reader.curves.push_back(MakeCurveRow());

  GrpcReplayBatchExecutor executor(
      std::move(solver), std::move(risk), ledger.get(), {}, &replay_reader);
  const auto result = executor.ExecuteBatch(
      "ns-1",
      R"({"tolerance":1.0})",
      R"([{"symbol":"BTCUSDT","side":"buy","pL":58000,"pH":62000,"qrate":1,"qmax":4,"executionwindow":3600}])",
      "user-1",
      "USDT",
      MakeBatchWithHistoricalFillOnly());

  bool ok = true;
  ok &= Check(result.outcome == BatchOutcome::kOk, "batch should succeed");
  ok &= Check(solver_ptr->last_request.external_liquidity_size() == 1,
              "historical fill venue is used to load external curve");
  ok &= Check(solver_ptr->last_request.reference_prices().contains("BTC/USDT"),
              "historical clear price is used as replay reference fallback");
  return ok;
}

bool TestResidualNormAboveToleranceIsSoftFailure() {
  auto solver = std::make_unique<FakeSolverStub>();
  auto risk = std::make_unique<FakeRiskStub>();
  AddReplayFill(&solver->response, 2.0);
  auto ledger = MakeLedger();

  GrpcReplayBatchExecutor executor(std::move(solver), std::move(risk), ledger.get(), {});
  const auto result = executor.ExecuteBatch(
      "ns-1",
      R"({"tolerance":1.0})",
      R"([{"symbol":"BTCUSDT","side":"buy","pL":58000,"pH":62000,"qrate":1,"qmax":4}])",
      "user-1",
      "USDT",
      MakeBatch());

  bool ok = true;
  ok &= Check(result.outcome == BatchOutcome::kSoftFailure,
              "residual > tolerance must be soft failure");
  ok &= Check(result.error_code == "residual_norm_above_tolerance",
              "soft failure error_code");
  ok &= Check(result.risk_status == "soft", "soft failure risk_status");
  ok &= Check(result.batch_result_json.find("\"riskstatus\":\"soft\"") !=
                  std::string::npos,
              "batch_result_json carries soft risk_status");
  return ok;
}

bool TestSellImplementationShortfallUsesExecMinusMid() {
  auto solver = std::make_unique<FakeSolverStub>();
  auto risk = std::make_unique<FakeRiskStub>();
  AddReplayFill(&solver->response,
                0.5,
                fob::common::v1::SIDE_SELL,
                59900.0);
  auto ledger = MakeLedger();

  GrpcReplayBatchExecutor executor(std::move(solver), std::move(risk), ledger.get(), {});
  const auto result = executor.ExecuteBatch(
      "ns-1",
      R"({"tolerance":1.0})",
      R"([{"symbol":"BTCUSDT","side":"sell","pL":58000,"pH":62000,"qrate":1,"qmax":4}])",
      "user-1",
      "USDT",
      MakeBatch());

  bool ok = true;
  ok &= Check(result.outcome == BatchOutcome::kOk, "sell batch should succeed");
  ok &= Check(Near(result.is_value, -150.0),
              "IS sell = execPrice - mid = 59900 - 60050");
  ok &= Check(Near(result.vwap, 59900.0), "sell VWAP");
  return ok;
}

bool TestPreTradeRiskRejectSkipsSolverPostTradeAndLedger() {
  auto solver = std::make_unique<FakeSolverStub>();
  auto risk = std::make_unique<FakeRiskStub>();
  auto* solver_ptr = solver.get();
  auto* risk_ptr = risk.get();
  risk->pre_trade_response.set_decision(fob::risk::v1::RISK_DECISION_REJECT);
  risk->pre_trade_response.mutable_error()->set_code("MAX_NOTIONAL");
  risk->pre_trade_response.mutable_error()->set_message("max notional exceeded");
  AddReplayFill(&solver->response);
  auto ledger = MakeLedger();

  GrpcReplayBatchExecutor executor(std::move(solver), std::move(risk), ledger.get(), {});
  const auto result = executor.ExecuteBatch(
      "ns-1",
      R"({"tolerance":1.0})",
      R"([{"symbol":"BTCUSDT","side":"buy","pL":58000,"pH":62000,"qrate":1,"qmax":4}])",
      "user-1",
      "USDT",
      MakeBatch());

  bool ok = true;
  ok &= Check(result.outcome == BatchOutcome::kSoftFailure,
              "risk reject is a failed batch that lets session continue");
  ok &= Check(result.failure_component == cex::backtest::app::FailureComponent::kRisk,
              "risk reject component");
  ok &= Check(result.error_code == "risk_pre_trade_rejected",
              "risk reject error code");
  ok &= Check(result.risk_status == "rejected", "risk status rejected");
  ok &= Check(result.fills_applied == 0, "no fills applied on risk reject");
  ok &= Check(Near(result.fill_rate, 0.0), "fill rate zero on risk reject");
  ok &= Check(result.batch_result_json.find("\"riskstatus\":\"rejected\"") !=
                  std::string::npos,
              "AgentLog batch_result carries risk rejection");
  ok &= Check(solver_ptr->calls == 0, "solver not called after pre-trade reject");
  ok &= Check(risk_ptr->pre_trade_calls == 1, "pre-trade risk called");
  ok &= Check(risk_ptr->post_trade_calls == 0,
              "post-trade risk not called without solver result");
  ok &= Check(!ledger->GetLastStep("ns-1").has_value(),
              "shadow ledger state unchanged for rejected batch");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= TestExecutorCallsSolverRiskLedgerAndComputesMetrics();
  ok &= TestExecutorSendsHistoricalExternalLiquidityToSolver();
  ok &= TestExecutorUsesHistoricalFillFallbackWhenSnapshotsAreAbsent();
  ok &= TestResidualNormAboveToleranceIsSoftFailure();
  ok &= TestSellImplementationShortfallUsesExecMinusMid();
  ok &= TestPreTradeRiskRejectSkipsSolverPostTradeAndLedger();
  if (!ok) return 1;
  std::cout << "[OK] backtest_grpc_replay_batch_executor_test passed\n";
  return 0;
}
