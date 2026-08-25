#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "app/historical_batch_loader_port.hpp"
#include "app/replay_config_repository_port.hpp"
#include "app/replay_config_snapshot.hpp"
#include "app/run_replay_audit_batch_uc.hpp"
#include "app/shadow_namespace_uc.hpp"
#include "infra/in_memory_shadow_ledger.hpp"

namespace {

using cex::backtest::app::AuditBatchSnapshot;
using cex::backtest::app::AuditExecutionResult;
using cex::backtest::app::FailureComponent;
using cex::backtest::app::HistoricalBatch;
using cex::backtest::app::HistoricalBatchResultRow;
using cex::backtest::app::HistoricalFillRow;
using cex::backtest::app::HistoricalMarketdataSnapshotRow;
using cex::backtest::app::HistoricalRiskEventRow;
using cex::backtest::app::IAuditBatchExecutor;
using cex::backtest::app::IAuditHistoricalLoader;
using cex::backtest::app::IReplayConfigRepository;
using cex::backtest::app::ReplayConfigSnapshotBuilder;
using cex::backtest::app::RunReplayAuditBatch;
using cex::backtest::app::ShadowNamespaceInitializer;
using cex::backtest::app::StoredConfigDocument;
using cex::backtest::infra::InMemoryShadowLedger;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

struct FakeConfigRepo final : public IReplayConfigRepository {
  std::optional<StoredConfigDocument> Doc(const std::string& id) const {
    if (id.empty()) return std::nullopt;
    StoredConfigDocument d;
    d.id = id;
    d.version = 1;
    d.body_json = "{}";
    return d;
  }

  std::optional<StoredConfigDocument> GetSolverConfig(const std::string& id) override {
    return Doc(id);
  }
  std::optional<StoredConfigDocument> GetRiskLimits(const std::string& id) override {
    return Doc(id);
  }
  std::optional<StoredConfigDocument> GetFeeModel(const std::string& id) override {
    return Doc(id);
  }
  std::optional<StoredConfigDocument> GetRewardConfig(const std::string& id) override {
    return Doc(id);
  }
};

struct FakeAuditLoader final : public IAuditHistoricalLoader {
  std::vector<HistoricalBatchResultRow> batches;
  std::vector<HistoricalFillRow> fills;
  std::vector<HistoricalMarketdataSnapshotRow> snapshots;
  std::vector<HistoricalRiskEventRow> risk_events;

  std::vector<HistoricalBatchResultRow> LoadBatchResultsById(
      const std::string& batch_id) override {
    std::vector<HistoricalBatchResultRow> out;
    for (const auto& row : batches) {
      if (row.batch_id == batch_id) out.push_back(row);
    }
    return out;
  }

  std::vector<HistoricalFillRow> LoadFillsByBatchIds(
      const std::vector<std::string>& batch_ids) override {
    std::vector<HistoricalFillRow> out;
    for (const auto& row : fills) {
      for (const auto& batch_id : batch_ids) {
        if (row.batch_id == batch_id) out.push_back(row);
      }
    }
    return out;
  }

  std::vector<HistoricalMarketdataSnapshotRow> LoadMarketdataSnapshotsByEventTimes(
      const std::vector<int64_t>& event_time_ms) override {
    std::vector<HistoricalMarketdataSnapshotRow> out;
    for (const auto& row : snapshots) {
      for (const auto ts : event_time_ms) {
        if (row.event_time_ms == ts) out.push_back(row);
      }
    }
    return out;
  }

  std::vector<HistoricalRiskEventRow> LoadRiskEventsByBatchId(
      const std::string& batch_id) override {
    std::vector<HistoricalRiskEventRow> out;
    for (const auto& row : risk_events) {
      if (row.batch_id == batch_id) out.push_back(row);
    }
    return out;
  }
};

struct FakeAuditExecutor final : public IAuditBatchExecutor {
  AuditExecutionResult scripted;
  std::string seen_namespace_id;
  std::string seen_snapshot_json;
  std::string seen_batch_id;

  AuditExecutionResult ExecuteAuditBatch(
      const std::string& namespace_id,
      const std::string& session_config_snapshot_json,
      const HistoricalBatch& batch) override {
    seen_namespace_id = namespace_id;
    seen_snapshot_json = session_config_snapshot_json;
    seen_batch_id = batch.batch_result.batch_id;
    return scripted;
  }
};

HistoricalBatchResultRow MakeBatch(const std::string& batch_id,
                                   const std::string& clear_prices_json,
                                   const std::string& executed_rates_json,
                                   const double residual_norm,
                                   const uint32_t solve_time_ms,
                                   const uint32_t fills_count,
                                   const int64_t event_time_ms = 1000) {
  HistoricalBatchResultRow row;
  row.batch_id = batch_id;
  row.event_time_ms = event_time_ms;
  row.clear_prices_json = clear_prices_json;
  row.executed_rates_json = executed_rates_json;
  row.residual_norm = residual_norm;
  row.solve_time_ms = solve_time_ms;
  row.fills_count = fills_count;
  return row;
}

HistoricalFillRow MakeFill(const std::string& batch_id,
                           const std::string& order_id,
                           const double qty,
                           const double price) {
  HistoricalFillRow row;
  row.batch_id = batch_id;
  row.event_time_ms = 1000;
  row.order_id = order_id;
  row.user_id = "user-1";
  row.symbol = "BTCUSDT";
  row.base = "BTC";
  row.quote = "USDT";
  row.side = "buy";
  row.executed_qty = qty;
  row.price = price;
  row.executed_notional = qty * price;
  row.fee_amount = 0.1;
  row.fee_currency = "USDT";
  row.liquidity_source = "internal";
  return row;
}

HistoricalRiskEventRow MakeRiskEvent(const std::string& batch_id,
                                     const std::string& event_type) {
  HistoricalRiskEventRow row;
  row.event_id = "risk-1";
  row.event_time_ms = 1001;
  row.entity_id = "user-1";
  row.event_type = event_type;
  row.batch_id = batch_id;
  row.details_json = "{}";
  return row;
}

RunReplayAuditBatch::Request MakeRequest(const std::string& audit_run_id,
                                         const std::string& batch_id) {
  RunReplayAuditBatch::Request request;
  request.audit_run_id = audit_run_id;
  request.batch_id = batch_id;
  request.tracked_user_id = "user-1";
  request.reporting_currency = "USDT";
  request.config_request.solver_config_id = "solver-1";
  request.config_request.risk_limits_id = "risk-1";
  request.config_request.fee_model_id = "fee-1";
  request.config_request.reward_config_id = "reward-1";
  request.config_request.random_seed = 7;
  request.config_request.tolerance = 1e-6;
  return request;
}

RunReplayAuditBatch::Clock FixedClock() {
  return []() {
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1711111111222}};
  };
}

bool test_equivalent_result_ignores_solve_time_mismatch() {
  FakeConfigRepo config_repo;
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  InMemoryShadowLedger shadow_ledger;
  ShadowNamespaceInitializer shadow_init(&shadow_ledger);
  FakeAuditLoader loader;
  FakeAuditExecutor executor;

  loader.batches.push_back(MakeBatch(
      "batch-1", "{ \"BTCUSDT\":\"50000\" }", "{ \"ord-1\":\"0.5\" }", 0.000001, 11, 1));
  loader.fills.push_back(MakeFill("batch-1", "ord-1", 1.0, 50000.0));

  executor.scripted.ok = true;
  executor.scripted.snapshot.batch_id = "batch-1";
  executor.scripted.snapshot.clear_prices_json = "{\"BTCUSDT\":\"50000\"}";
  executor.scripted.snapshot.executed_rates_json = "{\"ord-1\":\"0.5\"}";
  executor.scripted.snapshot.fills.push_back(MakeFill("batch-1", "ord-1", 1.0, 50000.0));
  executor.scripted.snapshot.residual_norm = 0.000001;
  executor.scripted.snapshot.solve_time_ms = 99;
  executor.scripted.snapshot.risk_status = "ok";

  RunReplayAuditBatch::Dependencies deps;
  deps.config_builder = &snapshot_builder;
  deps.shadow_init = &shadow_init;
  deps.audit_loader = &loader;
  deps.audit_executor = &executor;

  RunReplayAuditBatch uc(deps, FixedClock());
  const auto result = uc.Run(MakeRequest("audit-1", "batch-1"));

  return Check(result.ok, "happy path must succeed") &&
         Check(result.equivalent, "solve_time mismatch must not break equivalence") &&
         Check(!result.diff.solve_time_ms.equivalent,
               "solve_time diff must still be reported") &&
         Check(result.diff.clear_prices.equivalent, "clear_prices equal") &&
         Check(result.diff.executed_rates.equivalent, "executed_rates equal") &&
         Check(result.diff.fills.equivalent, "fills equal") &&
         Check(result.diff.residual_norm.equivalent, "residual within tolerance") &&
         Check(result.diff.risk_status.equivalent, "risk_status equal") &&
         Check(executor.seen_batch_id == "batch-1", "executor sees batch_id") &&
         Check(!executor.seen_namespace_id.empty(), "executor sees namespace_id");
}

bool test_risk_status_mismatch_breaks_equivalence() {
  FakeConfigRepo config_repo;
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  InMemoryShadowLedger shadow_ledger;
  ShadowNamespaceInitializer shadow_init(&shadow_ledger);
  FakeAuditLoader loader;
  FakeAuditExecutor executor;

  loader.batches.push_back(MakeBatch(
      "batch-risk", "{\"BTCUSDT\":\"50000\"}", "{\"ord-1\":\"0.5\"}", 0.0, 12, 1));
  loader.fills.push_back(MakeFill("batch-risk", "ord-1", 1.0, 50000.0));
  loader.risk_events.push_back(MakeRiskEvent("batch-risk", "margin_call"));

  executor.scripted.ok = true;
  executor.scripted.snapshot.batch_id = "batch-risk";
  executor.scripted.snapshot.clear_prices_json = "{\"BTCUSDT\":\"50000\"}";
  executor.scripted.snapshot.executed_rates_json = "{\"ord-1\":\"0.5\"}";
  executor.scripted.snapshot.fills.push_back(MakeFill("batch-risk", "ord-1", 1.0, 50000.0));
  executor.scripted.snapshot.residual_norm = 0.0;
  executor.scripted.snapshot.solve_time_ms = 12;
  executor.scripted.snapshot.risk_status = "ok";

  RunReplayAuditBatch::Dependencies deps;
  deps.config_builder = &snapshot_builder;
  deps.shadow_init = &shadow_init;
  deps.audit_loader = &loader;
  deps.audit_executor = &executor;

  RunReplayAuditBatch uc(deps, FixedClock());
  const auto result = uc.Run(MakeRequest("audit-2", "batch-risk"));

  return Check(result.ok, "risk mismatch run succeeds") &&
         Check(!result.equivalent, "risk mismatch must break equivalence") &&
         Check(!result.diff.risk_status.equivalent, "risk diff flagged") &&
         Check(result.production_snapshot.risk_status == "hard",
               "production risk folded to hard");
}

bool test_missing_batch_returns_no_data() {
  FakeConfigRepo config_repo;
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  InMemoryShadowLedger shadow_ledger;
  ShadowNamespaceInitializer shadow_init(&shadow_ledger);
  FakeAuditLoader loader;
  FakeAuditExecutor executor;
  executor.scripted.ok = true;

  RunReplayAuditBatch::Dependencies deps;
  deps.config_builder = &snapshot_builder;
  deps.shadow_init = &shadow_init;
  deps.audit_loader = &loader;
  deps.audit_executor = &executor;

  RunReplayAuditBatch uc(deps, FixedClock());
  const auto result = uc.Run(MakeRequest("audit-3", "missing-batch"));

  return Check(!result.ok, "missing batch must fail") &&
         Check(result.error_code == "no_data", "error_code=no_data");
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_equivalent_result_ignores_solve_time_mismatch() && ok;
  ok = test_risk_status_mismatch_breaks_equivalence() && ok;
  ok = test_missing_batch_returns_no_data() && ok;

  if (ok) {
    std::cout << "[OK] backtest_run_replay_audit_batch_test passed" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
