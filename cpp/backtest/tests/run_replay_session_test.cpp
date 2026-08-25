#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include "app/historical_batch_loader_port.hpp"
#include "app/historical_batch_loader_uc.hpp"
#include "app/replay_config_repository_port.hpp"
#include "app/replay_config_snapshot.hpp"
#include "app/replay_orchestration_ports.hpp"
#include "app/replay_runtime_metrics.hpp"
#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"
#include "app/run_replay_session_uc.hpp"
#include "app/shadow_namespace_uc.hpp"
#include "infra/in_memory_shadow_ledger.hpp"

namespace {

using cex::backtest::app::AgentLogEntry;
using cex::backtest::app::AtomicCancellationToken;
using cex::backtest::app::BatchExecutionResult;
using cex::backtest::app::BatchOutcome;
using cex::backtest::app::FailureComponent;
using cex::backtest::app::HistoricalBatch;
using cex::backtest::app::HistoricalBatchLoaderUseCases;
using cex::backtest::app::HistoricalBatchResultRow;
using cex::backtest::app::HistoricalFillRow;
using cex::backtest::app::HistoricalMarketdataSnapshotRow;
using cex::backtest::app::IBatchExecutor;
using cex::backtest::app::IHistoricalBatchLoader;
using cex::backtest::app::IReplayConfigRepository;
using cex::backtest::app::ReplayConfigRequest;
using cex::backtest::app::ReplayConfigSnapshotBuilder;
using cex::backtest::app::ReplayLifecycleEvent;
using cex::backtest::app::ReplayLifecycleStatus;
using cex::backtest::app::ReplayProgressEvent;
using cex::backtest::app::ReplayRuntimeMetrics;
using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionListFilter;
using cex::backtest::app::ReplaySessionRepositoryPort;
using cex::backtest::app::ReplaySessionStatePatch;
using cex::backtest::app::ReplaySessionStatus;
using cex::backtest::app::ReplaySummary;
using cex::backtest::app::RunReplaySession;
using cex::backtest::app::ShadowNamespaceInitializer;
using cex::backtest::app::StoredConfigDocument;
using cex::backtest::infra::InMemoryShadowLedger;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// --- Fakes ---

struct FakeSessionRepo final : public ReplaySessionRepositoryPort {
  // Хранилище созданных сессий
  std::unordered_map<std::string, ReplaySession> storage;
  std::vector<std::pair<std::string, ReplaySessionStatePatch>> patches;
  int get_calls{0};
  int cancel_on_get_call{0};
  
  ReplaySession Create(const ReplaySession& s) override { 
    storage[s.session_id] = s;
    return s; 
  }
  
  std::optional<ReplaySession> GetById(const std::string& id) override { 
    ++get_calls;
    auto it = storage.find(id);
    if (it != storage.end()) {
      if (cancel_on_get_call > 0 && get_calls == cancel_on_get_call) {
        it->second.status = ReplaySessionStatus::kCancelled;
        it->second.error_details = "cancelled during preparation";
      }
      return it->second;
    }
    return std::nullopt;
  }
  
  std::vector<ReplaySession> List(const ReplaySessionListFilter& filter) override { 
    std::vector<ReplaySession> out;
    for (const auto& [_, s] : storage) {
      // Применяем фильтры
      if (filter.user_id.has_value() && s.user_id != *filter.user_id) continue;
      if (filter.status.has_value() && s.status != *filter.status) continue;
      if (filter.created_from.has_value() && s.created_at < *filter.created_from) continue;
      if (filter.created_to.has_value() && s.created_at > *filter.created_to) continue;
      if (filter.replay_from.has_value() && s.date_range_from < *filter.replay_from) continue;
      if (filter.replay_to.has_value() && s.date_range_to > *filter.replay_to) continue;
      out.push_back(s);
    }
    
    // Сортируем по created_at DESC
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
      return a.created_at > b.created_at;
    });
    
    // Применяем limit/offset
    if (filter.limit.has_value()) {
      size_t start = filter.offset.has_value() ? *filter.offset : 0;
      size_t end = start + *filter.limit;
      if (start < out.size()) {
        out = std::vector<ReplaySession>(
            out.begin() + std::min(start, out.size()),
            out.begin() + std::min(end, out.size()));
      }
    }
    
    return out;
  }
  
  bool UpdateState(const std::string& session_id,
                   const ReplaySessionStatePatch& patch) override {
    auto it = storage.find(session_id);
    if (it == storage.end()) return false;
    
    patches.emplace_back(session_id, patch);
    
    // Применяем патч к хранимой сессии
    ReplaySession& s = it->second;
    if (patch.status.has_value()) {
      s.status = patch.status.value();
    }
    if (patch.started_at.has_value()) {
      s.started_at = patch.started_at;
    }
    if (patch.completed_at.has_value()) {
      s.completed_at = patch.completed_at;
    }
    if (patch.total_batches.has_value()) {
      s.total_batches = patch.total_batches;
    }
    if (patch.progress_batches.has_value()) {
      s.progress_batches = patch.progress_batches.value();
    }
    if (patch.error_details.has_value()) {
      s.error_details = patch.error_details;
    }
    return true;
  }

  std::vector<ReplaySession> GetRetryChain(const std::string&) override {
    return {};
  }

  std::optional<ReplaySessionStatus> LastStatus() const {
    for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
      if (it->second.status.has_value()) return it->second.status;
    }
    return std::nullopt;
  }
};

// Вспомогательная функция для создания тестовой сессии
ReplaySession MakeTestSession(const std::string& session_id, 
                               const std::string& user_id = "test-user",
                               std::int64_t created_at_sec = 1700000000) {
  ReplaySession session;
  session.session_id = session_id;
  session.user_id = user_id;
  session.name = "test-session";
  session.strategy_json = "{}";
  session.date_range_from = std::chrono::system_clock::time_point{std::chrono::seconds{0}};
  session.date_range_to = std::chrono::system_clock::time_point{std::chrono::seconds{1000}};
  session.solver_config_id = "solver-config-1";
  session.risk_limits_id = "risk-limits-1";
  session.fee_model_json = "{}";
  session.session_config_snapshot_json = std::nullopt;
  session.status = ReplaySessionStatus::kPending;
  session.total_batches = std::nullopt;
  session.progress_batches = 0;
  session.created_at = std::chrono::system_clock::time_point{std::chrono::seconds{created_at_sec}};
  session.started_at = std::nullopt;
  session.completed_at = std::nullopt;
  session.error_details = std::nullopt;
  return session;
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
  std::optional<StoredConfigDocument> GetSolverConfig(const std::string& id) override { return Doc(id); }
  std::optional<StoredConfigDocument> GetRiskLimits(const std::string& id) override { return Doc(id); }
  std::optional<StoredConfigDocument> GetFeeModel(const std::string& id) override { return Doc(id); }
  std::optional<StoredConfigDocument> GetRewardConfig(const std::string& id) override { return Doc(id); }
};

struct FakeHistoricalLoader final : public IHistoricalBatchLoader {
  std::vector<HistoricalBatchResultRow> batches;
  std::vector<HistoricalFillRow> fills;
  std::vector<HistoricalMarketdataSnapshotRow> snapshots;

  std::vector<HistoricalBatchResultRow> LoadBatchResults(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    std::vector<HistoricalBatchResultRow> out;
    for (const auto& row : batches) {
      if (row.event_time_ms < from_ms || row.event_time_ms > to_ms) continue;
      out.push_back(row);
    }
    std::vector<HistoricalBatchResultRow> paged;
    for (int64_t i = offset;
         i < static_cast<int64_t>(out.size()) && i < offset + limit; ++i) {
      paged.push_back(out[static_cast<std::size_t>(i)]);
    }
    return paged;
  }

  std::vector<HistoricalFillRow> LoadFills(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    std::vector<HistoricalFillRow> out;
    for (const auto& row : fills) {
      if (row.event_time_ms < from_ms || row.event_time_ms > to_ms) continue;
      out.push_back(row);
    }
    std::vector<HistoricalFillRow> paged;
    for (int64_t i = offset;
         i < static_cast<int64_t>(out.size()) && i < offset + limit; ++i) {
      paged.push_back(out[static_cast<std::size_t>(i)]);
    }
    return paged;
  }

  std::vector<HistoricalMarketdataSnapshotRow> LoadMarketdataSnapshots(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    std::vector<HistoricalMarketdataSnapshotRow> out;
    for (const auto& row : snapshots) {
      if (row.event_time_ms < from_ms || row.event_time_ms > to_ms) continue;
      out.push_back(row);
    }
    std::vector<HistoricalMarketdataSnapshotRow> paged;
    for (int64_t i = offset;
         i < static_cast<int64_t>(out.size()) && i < offset + limit; ++i) {
      paged.push_back(out[static_cast<std::size_t>(i)]);
    }
    return paged;
  }
};

struct FakeExecutor final : public IBatchExecutor {
  std::vector<BatchExecutionResult> scripted;
  std::vector<std::string> seen_namespace;
  std::vector<std::string> seen_batch_id;
  std::vector<std::string> seen_snapshot_json;
  std::vector<std::string> seen_strategy_json;
  std::vector<std::string> seen_tracked_user_id;
  std::vector<std::string> seen_reporting_currency;
  size_t cursor{0};

  BatchExecutionResult ExecuteBatch(const std::string& namespace_id,
                                    const std::string& snapshot_json,
                                    const std::string& strategy_json,
                                    const std::string& tracked_user_id,
                                    const std::string& reporting_currency,
                                    const HistoricalBatch& batch) override {
    seen_namespace.push_back(namespace_id);
    seen_batch_id.push_back(batch.batch_result.batch_id);
    seen_snapshot_json.push_back(snapshot_json);
    seen_strategy_json.push_back(strategy_json);
    seen_tracked_user_id.push_back(tracked_user_id);
    seen_reporting_currency.push_back(reporting_currency);
    if (cursor < scripted.size()) return scripted[cursor++];
    return BatchExecutionResult{};
  }
};

struct FakeAgentLogWriter final : public cex::backtest::app::IAgentLogWriter {
  std::vector<AgentLogEntry> entries;
  int calls{0};
  int fail_on_call{-1};
  void WriteAgentLog(const AgentLogEntry& e) override {
    ++calls;
    if (fail_on_call == calls) {
      throw std::runtime_error("ClickHouse replay_agentlogs insert failed");
    }
    for (auto& existing : entries) {
      if (existing.session_id == e.session_id &&
          existing.original_batch_id == e.original_batch_id) {
        existing = e;
        return;
      }
    }
    entries.push_back(e);
  }
};

struct FakeSummaryStore final : public cex::backtest::app::IReplaySummaryStore {
  std::optional<ReplaySummary> last;
  void SaveSummary(const ReplaySummary& s) override { last = s; }
};

struct FakeEventPublisher final : public cex::backtest::app::IReplayEventPublisher {
  std::vector<ReplayProgressEvent> progress;
  std::vector<ReplayLifecycleEvent> lifecycle;
  void PublishProgress(const ReplayProgressEvent& e) override { progress.push_back(e); }
  void PublishLifecycle(const ReplayLifecycleEvent& e) override { lifecycle.push_back(e); }
};

struct Harness {
  FakeConfigRepo config_repo;
  ReplayConfigSnapshotBuilder config_builder{&config_repo};
  ReplayRuntimeMetrics runtime_metrics;
  InMemoryShadowLedger shadow_ledger{&runtime_metrics};
  ShadowNamespaceInitializer shadow_init{&shadow_ledger};
  FakeHistoricalLoader history;
  HistoricalBatchLoaderUseCases history_uc{&history};
  FakeSessionRepo session_repo;
  FakeExecutor executor;
  FakeAgentLogWriter agent_log;
  FakeSummaryStore summary_store;
  FakeEventPublisher publisher;

  RunReplaySession::Dependencies Deps() {
    RunReplaySession::Dependencies d;
    d.session_repo = &session_repo;
    d.config_builder = &config_builder;
    d.shadow_init = &shadow_init;
    d.shadow_ledger = &shadow_ledger;
    d.history_loader = &history_uc;
    d.batch_executor = &executor;
    d.agent_log_writer = &agent_log;
    d.summary_store = &summary_store;
    d.event_publisher = &publisher;
    d.runtime_metrics = &runtime_metrics;
    return d;
  }
  
  // Вспомогательный метод для создания сессии в репозитории
  void CreateSession(const std::string& session_id) {
    auto session = MakeTestSession(session_id);
    session_repo.Create(session);
  }
};

RunReplaySession::Request MakeReq(const std::string& session_id,
                                  size_t batch_count) {
  RunReplaySession::Request r;
  r.session_id = session_id;
  r.tracked_user_id = "replay-user";
  r.reporting_currency = "USDT";
  r.config_request.solver_config_id = "solver";
  r.config_request.risk_limits_id = "risk";
  r.config_request.fee_model_id = "fee";
  r.config_request.reward_config_id = "reward";
  r.config_request.random_seed = 1;
  r.history_config.from_ms = 0;
  r.history_config.to_ms = 1000;
  r.history_config.chunk_size = 100;
  (void)batch_count;
  return r;
}

void SeedBatches(Harness& h, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    HistoricalBatchResultRow r;
    r.batch_id = "b" + std::to_string(i);
    r.event_time_ms = static_cast<int64_t>(100 * (i + 1));
    h.history.batches.push_back(r);
  }
}

HistoricalFillRow MakeFill(const std::string& batch_id,
                           int64_t event_time_ms,
                           const std::string& order_id) {
  HistoricalFillRow row;
  row.batch_id = batch_id;
  row.event_time_ms = event_time_ms;
  row.order_id = order_id;
  row.user_id = "replay-user";
  row.symbol = "BTC/USDT";
  row.base = "BTC";
  row.quote = "USDT";
  row.side = "buy";
  row.executed_qty = 1.0;
  row.price = 50000.0;
  row.executed_notional = 50000.0;
  row.liquidity_source = "internal";
  return row;
}

BatchExecutionResult MakeExec(BatchOutcome outcome,
                              double pnl = 0.0,
                              double is_value = 0.0,
                              double fill_rate = 0.0,
                              uint32_t fills_applied = 0,
                              uint32_t solve_time_ms = 0,
                              std::string error_details = {},
                              std::string error_code = {},
                              FailureComponent failure_component = FailureComponent::kUnknown) {
  BatchExecutionResult result;
  result.outcome = outcome;
  result.pnl = pnl;
  result.is_value = is_value;
  result.fill_rate = fill_rate;
  result.fills_applied = fills_applied;
  result.solve_time_ms = solve_time_ms;
  result.error_details = std::move(error_details);
  result.error_code = std::move(error_code);
  result.failure_component = failure_component;
  return result;
}

RunReplaySession::Clock FixedClock() {
  return []() {
    return std::chrono::system_clock::time_point{std::chrono::seconds{1700000000}};
  };
}

bool test_completed_happy_path() {
  Harness h;
  h.CreateSession("sess-1");  // <-- СОЗДАЕМ СЕССИЮ
  SeedBatches(h, 3);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 10.0, 0.5, 0.9, 2, 5),
      MakeExec(BatchOutcome::kOk, -3.0, 0.6, 0.8, 1, 4),
      MakeExec(BatchOutcome::kOk, 7.0, 0.4, 0.7, 1, 6),
  };

  RunReplaySession uc(h.Deps(), FixedClock());
  auto result = uc.Run(MakeReq("sess-1", 3));

  if (!Check(result.final_status == ReplaySessionStatus::kCompleted, "completed")) return false;
  if (!Check(result.summary.processed_batches == 3, "processed=3")) return false;
  if (!Check(result.summary.failed_batches == 0, "failed=0")) return false;
  if (!Check(result.summary.total_pnl == 14.0, "total pnl 14")) return false;
  if (!Check(std::abs(result.summary.avg_pnl - 14.0 / 3.0) < 1e-9, "avg pnl")) return false;
  // peak after batch1 = 10, dip to 7, recover to 14 -> max drawdown = 3.
  if (!Check(std::abs(result.summary.max_drawdown - 3.0) < 1e-9, "max drawdown=3")) return false;
  if (!Check(!result.summary.partial, "not partial")) return false;
  if (!Check(h.summary_store.last.has_value(), "summary saved")) return false;
  if (!Check(h.agent_log.entries.size() == 3, "agent log per batch")) return false;
  if (!Check(h.publisher.progress.size() == 3, "progress per batch")) return false;
  // first lifecycle = running, last = completed.
  if (!Check(h.publisher.lifecycle.front().status == ReplayLifecycleStatus::kRunning,
             "first lifecycle running")) return false;
  if (!Check(h.publisher.lifecycle.back().status == ReplayLifecycleStatus::kCompleted,
             "last lifecycle completed")) return false;
  // session repo got pending -> running -> per-batch progress -> completed.
  if (!Check(h.session_repo.LastStatus() == ReplaySessionStatus::kCompleted,
             "session ends in completed")) return false;
  return true;
}

bool test_status_transitions_pending_running_completed() {
  Harness h;
  h.CreateSession("sess-tx");  // <-- СОЗДАЕМ СЕССИЮ
  SeedBatches(h, 2);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 1),
      MakeExec(BatchOutcome::kOk, 2),
  };
  RunReplaySession uc(h.Deps(), FixedClock());
  uc.Run(MakeReq("sess-tx", 2));

  std::vector<ReplaySessionStatus> statuses;
  for (const auto& [_, p] : h.session_repo.patches) {
    if (p.status.has_value()) statuses.push_back(*p.status);
  }
  // First status must be running, last must be completed; failed/cancelled not present.
  if (!Check(!statuses.empty() && statuses.front() == ReplaySessionStatus::kRunning,
             "first status = running")) return false;
  if (!Check(statuses.back() == ReplaySessionStatus::kCompleted,
             "last status = completed")) return false;
  for (auto s : statuses) {
    if (s == ReplaySessionStatus::kFailed || s == ReplaySessionStatus::kCancelled) {
      return Check(false, "no failed/cancelled in completed run");
    }
  }
  return true;
}

bool test_hard_failure_marks_failed_and_partial() {
  Harness h;
  h.CreateSession("sess-hard");  // <-- СОЗДАЕМ СЕССИЮ
  SeedBatches(h, 4);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 5),
      MakeExec(BatchOutcome::kHardFailure,
               0,
               0,
               0,
               0,
               0,
               "ledger broke",
               "ledger_apply_failed",
               FailureComponent::kLedger),
      MakeExec(BatchOutcome::kOk, 99),  // must not be reached
  };

  RunReplaySession uc(h.Deps(), FixedClock());
  auto result = uc.Run(MakeReq("sess-hard", 4));

  if (!Check(result.final_status == ReplaySessionStatus::kFailed, "failed")) return false;
  if (!Check(Contains(result.error_details, "ledger_apply_failed"),
             "error code propagated: " + result.error_details))
    return false;
  if (!Check(Contains(result.error_details, "ledger"),
             "error details propagated: " + result.error_details))
    return false;
  if (!Check(result.summary.processed_batches == 1, "only 1 batch processed before hard")) return false;
  if (!Check(result.summary.failed_batches == 1, "1 failed batch counted")) return false;
  if (!Check(result.summary.partial, "summary marked partial")) return false;
  if (!Check(h.executor.cursor == 2, "executor stopped after hard failure")) return false;
  // agent log contains both ok and hard entries.
  if (!Check(h.agent_log.entries.size() == 2, "agent log written for ok + hard")) return false;
  if (!Check(h.agent_log.entries.back().risk_status == "hard", "last log = hard")) return false;
  if (!Check(h.agent_log.entries.back().error_code == "ledger_apply_failed",
             "hard error code logged"))
    return false;
  if (!Check(h.agent_log.entries.back().failure_component == FailureComponent::kLedger,
             "hard component logged"))
    return false;
  return true;
}

bool test_agent_log_write_failure_marks_failed_and_partial() {
  Harness h;
  h.CreateSession("sess-log-write");
  SeedBatches(h, 3);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 10),
      MakeExec(BatchOutcome::kOk, 20),
      MakeExec(BatchOutcome::kOk, 30),
  };
  h.agent_log.fail_on_call = 2;

  RunReplaySession uc(h.Deps(), FixedClock());
  auto result = uc.Run(MakeReq("sess-log-write", 3));

  if (!Check(result.final_status == ReplaySessionStatus::kFailed,
             "AgentLog write failure marks session failed")) return false;
  if (!Check(Contains(result.error_details, "agentlog_write_failed"),
             "AgentLog write error code propagated")) return false;
  if (!Check(Contains(result.error_details, "AgentLog write failure"),
             "AgentLog write failure is explicit")) return false;
  if (!Check(Contains(result.error_details, "ClickHouse replay_agentlogs insert failed"),
             "AgentLog write exception details propagated")) return false;
  if (!Check(result.summary.processed_batches == 1,
             "summary includes only batches with persisted AgentLog")) return false;
  if (!Check(result.summary.failed_batches == 0,
             "failed current batch is excluded when AgentLog was not persisted")) return false;
  if (!Check(result.summary.partial, "summary marked partial after AgentLog failure"))
    return false;
  if (!Check(h.agent_log.entries.size() == 1,
             "previously persisted AgentLog remains available")) return false;
  if (!Check(h.summary_store.last.has_value() && h.summary_store.last->partial,
             "partial summary saved after AgentLog failure")) return false;
  if (!Check(h.session_repo.LastStatus().has_value() &&
                 *h.session_repo.LastStatus() == ReplaySessionStatus::kFailed,
             "failed status persisted after AgentLog failure")) return false;
  if (!Check(!h.publisher.lifecycle.empty() &&
                 h.publisher.lifecycle.back().error_code.has_value() &&
                 *h.publisher.lifecycle.back().error_code == "agentlog_write_failed",
             "failed lifecycle event carries AgentLog error code")) return false;
  return true;
}

bool test_soft_failures_continue_session() {
  Harness h;
  h.CreateSession("sess-soft");
  SeedBatches(h, 5);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 10),
      MakeExec(BatchOutcome::kSoftFailure,
               0,
               0,
               0,
               0,
               0,
               "solver diverged",
               "solver_diverged",
               FailureComponent::kSolver),
      MakeExec(BatchOutcome::kSoftFailure,
               0,
               0,
               0,
               0,
               0,
               "marketdata snapshot missing",
               "marketdata_missing",
               FailureComponent::kMarketData),
      MakeExec(BatchOutcome::kSoftFailure,
               0,
               0,
               0,
               0,
               0,
               "local risk alert",
               "risk_alert",
               FailureComponent::kRisk),
      MakeExec(BatchOutcome::kOk, 5),
  };
  RunReplaySession uc(h.Deps(), FixedClock());
  auto result = uc.Run(MakeReq("sess-soft", 5));

  if (!Check(result.final_status == ReplaySessionStatus::kCompleted, "soft -> completed")) return false;
  if (!Check(result.summary.processed_batches == 2, "2 ok batches processed")) return false;
  if (!Check(result.summary.failed_batches == 3, "3 soft failures counted")) return false;
  if (!Check(result.summary.partial, "completed run with soft failures is partial")) return false;
  if (!Check(h.agent_log.entries.size() == 5, "log written for all 5 batches incl soft"))
    return false;
  if (!Check(h.agent_log.entries[1].risk_status == "soft", "soft batch flagged in log"))
    return false;
  if (!Check(h.agent_log.entries[1].error_code == "solver_diverged",
             "solver soft code logged"))
    return false;
  if (!Check(h.agent_log.entries[1].failure_component == FailureComponent::kSolver,
             "solver soft component logged"))
    return false;
  if (!Check(h.agent_log.entries[2].error_code == "marketdata_missing",
             "marketdata soft code logged"))
    return false;
  if (!Check(h.agent_log.entries[2].failure_component == FailureComponent::kMarketData,
             "marketdata soft component logged"))
    return false;
  if (!Check(h.agent_log.entries[3].error_code == "risk_alert",
             "risk soft code logged"))
    return false;
  if (!Check(h.agent_log.entries[3].failure_component == FailureComponent::kRisk,
             "risk soft component logged"))
    return false;
  if (!Check(h.publisher.progress.size() == 5, "progress still emitted for soft failures"))
    return false;
  return true;
}

bool test_reward_policy_incremental_pnl_with_failures_zeroed() {
  Harness h;
  h.CreateSession("sess-reward");
  SeedBatches(h, 3);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 4.5),
      MakeExec(BatchOutcome::kSoftFailure, 11.0),
      MakeExec(BatchOutcome::kHardFailure, 22.0, 0, 0, 0, 0, "hard", "hard", FailureComponent::kSolver),
  };

  RunReplaySession uc(h.Deps(), FixedClock());
  (void)uc.Run(MakeReq("sess-reward", 3));

  if (!Check(h.agent_log.entries.size() == 3, "all executed batches logged")) return false;
  if (!Check(std::abs(h.agent_log.entries[0].reward - 4.5) < 1e-9, "ok reward equals pnl"))
    return false;
  if (!Check(std::abs(h.agent_log.entries[1].reward - 0.0) < 1e-9, "soft reward is zero"))
    return false;
  if (!Check(std::abs(h.agent_log.entries[2].reward - 0.0) < 1e-9, "hard reward is zero"))
    return false;
  return true;
}

bool test_reward_policy_negative_is_from_snapshot() {
  Harness h;
  h.CreateSession("sess-reward-is");
  h.session_repo.storage["sess-reward-is"].session_config_snapshot_json =
      R"({"snapshot_version":1,"reward_config":{"body_json":"{\"mode\":\"-IS\"}"}})";
  SeedBatches(h, 3);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 4.5, 1.25),
      MakeExec(BatchOutcome::kOk, 7.0, -2.0),
      MakeExec(BatchOutcome::kSoftFailure, 11.0, 9.0),
  };

  RunReplaySession uc(h.Deps(), FixedClock());
  const auto result = uc.Run(MakeReq("sess-reward-is", 3));

  if (!Check(result.final_status == ReplaySessionStatus::kCompleted,
             "-IS reward run completes")) return false;
  if (!Check(h.agent_log.entries.size() == 3, "all batches logged")) return false;
  if (!Check(std::abs(h.agent_log.entries[0].reward + 1.25) < 1e-9,
             "-IS reward uses negative IS for positive shortfall")) return false;
  if (!Check(std::abs(h.agent_log.entries[1].reward - 2.0) < 1e-9,
             "-IS reward rewards negative shortfall")) return false;
  if (!Check(std::abs(h.agent_log.entries[2].reward - 0.0) < 1e-9,
             "-IS reward zeroes failed batches")) return false;
  return true;
}

bool test_unsupported_reward_policy_fails_before_batches() {
  Harness h;
  h.CreateSession("sess-reward-unsupported");
  h.session_repo.storage["sess-reward-unsupported"].session_config_snapshot_json =
      R"({"snapshot_version":1,"reward_config":{"body_json":"{\"mode\":\"hybrid\"}"}})";
  SeedBatches(h, 2);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 4.5, 1.25),
      MakeExec(BatchOutcome::kOk, 7.0, -2.0),
  };

  RunReplaySession uc(h.Deps(), FixedClock());
  const auto result = uc.Run(MakeReq("sess-reward-unsupported", 2));

  if (!Check(result.final_status == ReplaySessionStatus::kFailed,
             "unsupported reward mode fails session")) return false;
  if (!Check(Contains(result.error_details, "unsupported_reward_mode"),
             "unsupported reward error surfaced")) return false;
  if (!Check(h.executor.seen_batch_id.empty(),
             "unsupported reward mode must not execute batches")) return false;
  return Check(h.agent_log.entries.empty(),
               "unsupported reward mode must not write AgentLog");
}

bool test_materialized_snapshot_passed_to_executor() {
  Harness h;
  h.CreateSession("sess-snapshot");
  SeedBatches(h, 2);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 1.0),
      MakeExec(BatchOutcome::kOk, 1.0),
  };
  auto req = MakeReq("sess-snapshot", 2);
  req.config_request.solver_config_inline_override = R"({"alpha":0.99})";
  req.config_request.random_seed = 123;
  req.config_request.tolerance = 1e-4;

  RunReplaySession uc(h.Deps(), FixedClock());
  (void)uc.Run(req);

  if (!Check(h.executor.seen_snapshot_json.size() == 2, "snapshot passed on each batch"))
    return false;
  if (!Check(h.executor.seen_snapshot_json[0] == h.executor.seen_snapshot_json[1],
             "materialized snapshot must be stable across batches"))
    return false;
  if (!Check(Contains(h.executor.seen_snapshot_json[0], "\"snapshot_version\":1"),
             "snapshot has version")) return false;
  if (!Check(Contains(h.executor.seen_snapshot_json[0], "\"random_seed\":123"),
             "snapshot has configured seed")) return false;
  if (!Check(Contains(h.executor.seen_snapshot_json[0], "\"tolerance\":"),
             "snapshot has tolerance")) return false;
  return true;
}

bool test_session_strategy_passed_to_executor() {
  Harness h;
  h.CreateSession("sess-strategy");
  const std::string strategy =
      R"([{"symbol":"BTCUSDT","side":"buy","pL":58000,"pH":62000,"qrate":0.5,"qmax":100,"executionwindow":3600}])";
  h.session_repo.storage["sess-strategy"].strategy_json = strategy;
  SeedBatches(h, 1);
  h.executor.scripted = {MakeExec(BatchOutcome::kOk, 1.0)};

  RunReplaySession uc(h.Deps(), FixedClock());
  const auto result = uc.Run(MakeReq("sess-strategy", 1));

  if (!Check(result.final_status == ReplaySessionStatus::kCompleted,
             "strategy run completed")) return false;
  if (!Check(h.executor.seen_strategy_json.size() == 1,
             "strategy passed once")) return false;
  if (!Check(h.executor.seen_strategy_json.front() == strategy,
             "executor receives persisted session strategy")) return false;
  return true;
}

bool test_persisted_session_snapshot_reused_for_replay() {
  Harness h;
  h.CreateSession("sess-stored-snapshot");
  h.session_repo.storage["sess-stored-snapshot"].session_config_snapshot_json =
      R"({"snapshot_version":1,"source":"stored-session-snapshot"})";
  SeedBatches(h, 1);
  h.executor.scripted = {MakeExec(BatchOutcome::kOk, 1.0)};

  auto req = MakeReq("sess-stored-snapshot", 1);
  req.config_request = ReplayConfigRequest{};

  RunReplaySession uc(h.Deps(), FixedClock());
  auto result = uc.Run(req);

  if (!Check(result.final_status == ReplaySessionStatus::kCompleted,
             "run should complete from persisted snapshot")) return false;
  if (!Check(h.executor.seen_snapshot_json.size() == 1,
             "executor should receive one snapshot")) return false;
  if (!Check(h.executor.seen_snapshot_json[0] ==
                 R"({"snapshot_version":1,"source":"stored-session-snapshot"})",
             "executor should receive persisted session snapshot")) return false;
  return true;
}

bool test_executor_payloads_are_persisted_to_agent_log() {
  Harness h;
  h.CreateSession("sess-payloads");
  SeedBatches(h, 1);

  auto exec = MakeExec(BatchOutcome::kOk, 3.0, 1.1, 50.0, 2, 8);
  exec.state_json = R"({"positions":[{"symbol":"BTCUSDT","qty":1.0}]})";
  exec.action_json = R"([{"symbol":"BTCUSDT","side":"buy","qmax":100}])";
  exec.fills_json = R"([{"price":60100,"execqty":0.5}])";
  exec.batch_result_json =
      R"({"clearprices":{"BTCUSDT":60100},"residualnorm":0.01})";
  exec.metrics_json = R"({"cumPnL":3.0,"fillrate":50.0})";
  exec.risk_status = "alert";
  h.executor.scripted = {exec};

  RunReplaySession uc(h.Deps(), FixedClock());
  const auto result = uc.Run(MakeReq("sess-payloads", 1));

  if (!Check(result.final_status == ReplaySessionStatus::kCompleted,
             "payload run completed")) return false;
  if (!Check(h.agent_log.entries.size() == 1, "one agent log written")) return false;
  const auto& log = h.agent_log.entries.front();
  if (!Check(log.state_json == exec.state_json, "state_json propagated")) return false;
  if (!Check(log.action_json == exec.action_json, "action_json propagated")) return false;
  if (!Check(log.fills_json == exec.fills_json, "fills_json propagated")) return false;
  if (!Check(log.batch_result_json == exec.batch_result_json,
             "batch_result_json propagated")) return false;
  if (!Check(log.metrics_json == exec.metrics_json, "metrics_json propagated")) return false;
  if (!Check(log.risk_status == "alert", "risk_status propagated")) return false;
  return true;
}

bool test_cancellation_stops_loop() {
  Harness h;
  h.CreateSession("sess-cxl");  // <-- СОЗДАЕМ СЕССИЮ
  SeedBatches(h, 5);
  AtomicCancellationToken token;
  // Cancel after first batch via custom executor that flips token mid-run.
  struct CancellingExecutor final : public IBatchExecutor {
    AtomicCancellationToken* tok;
    int calls{0};
    BatchExecutionResult ExecuteBatch(const std::string&, const std::string&,
                                       const std::string&, const std::string&,
                                       const std::string&,
                                       const HistoricalBatch&) override {
      ++calls;
      if (calls == 1) tok->Cancel();
      return MakeExec(BatchOutcome::kOk, 1.0);
    }
  };
  CancellingExecutor exec;
  exec.tok = &token;

  auto deps = h.Deps();
  deps.batch_executor = &exec;
  deps.cancellation_token = &token;

  RunReplaySession uc(deps, FixedClock());
  auto result = uc.Run(MakeReq("sess-cxl", 5));

  if (!Check(result.final_status == ReplaySessionStatus::kCancelled, "cancelled"))
    return false;
  if (!Check(result.summary.partial, "partial when cancelled mid-run")) return false;
  if (!Check(exec.calls == 1, "only one batch executed before cancel")) return false;
  if (!Check(h.publisher.lifecycle.back().status == ReplayLifecycleStatus::kCancelled,
             "lifecycle ends in cancelled")) return false;
  return true;
}

bool test_terminal_session_is_not_restarted() {
  Harness h;
  h.CreateSession("sess-terminal");
  auto& session = h.session_repo.storage["sess-terminal"];
  session.status = ReplaySessionStatus::kCancelled;
  session.total_batches = 10;
  session.progress_batches = 3;
  session.error_details = "cancelled by user";
  SeedBatches(h, 2);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 99.0),
      MakeExec(BatchOutcome::kOk, 99.0),
  };

  RunReplaySession uc(h.Deps(), FixedClock());
  auto result = uc.Run(MakeReq("sess-terminal", 2));

  if (!Check(result.final_status == ReplaySessionStatus::kCancelled,
             "terminal cancelled status preserved")) return false;
  if (!Check(result.summary.total_batches == 10, "stored total_batches preserved"))
    return false;
  if (!Check(result.summary.processed_batches == 3,
             "stored progress_batches preserved")) return false;
  if (!Check(result.summary.partial, "terminal cancelled summary is partial"))
    return false;
  if (!Check(result.error_details == "cancelled by user",
             "stored terminal error propagated")) return false;
  if (!Check(h.session_repo.patches.empty(),
             "terminal session must not be patched to running")) return false;
  if (!Check(h.executor.seen_batch_id.empty(),
             "terminal session must not execute batches")) return false;
  if (!Check(h.agent_log.entries.empty(),
             "terminal session must not write new agent logs")) return false;
  if (!Check(h.summary_store.last == std::nullopt,
             "terminal session skip must not overwrite summary")) return false;
  if (!Check(h.publisher.progress.empty() && h.publisher.lifecycle.empty(),
             "terminal session skip must not publish events")) return false;
  return true;
}

bool test_cancel_token_before_start_does_not_promote_to_running() {
  Harness h;
  h.CreateSession("sess-token-prestart");
  h.session_repo.storage["sess-token-prestart"].total_batches = 7;
  h.session_repo.storage["sess-token-prestart"].progress_batches = 2;
  SeedBatches(h, 2);
  AtomicCancellationToken token;
  token.Cancel();

  auto deps = h.Deps();
  deps.cancellation_token = &token;

  RunReplaySession uc(deps, FixedClock());
  auto result = uc.Run(MakeReq("sess-token-prestart", 2));

  if (!Check(result.final_status == ReplaySessionStatus::kCancelled,
             "pre-start token -> cancelled")) return false;
  if (!Check(result.summary.total_batches == 7,
             "pre-start cancellation keeps stored total")) return false;
  if (!Check(result.summary.processed_batches == 2,
             "pre-start cancellation keeps stored progress")) return false;
  if (!Check(result.summary.partial, "pre-start cancellation is partial")) return false;
  if (!Check(h.executor.seen_batch_id.empty(),
             "pre-start cancellation must not execute batches")) return false;
  for (const auto& [_, patch] : h.session_repo.patches) {
    if (patch.status.has_value() && *patch.status == ReplaySessionStatus::kRunning) {
      return Check(false, "pre-start cancellation must not patch running");
    }
  }
  if (!Check(!h.publisher.lifecycle.empty() &&
                 h.publisher.lifecycle.back().status == ReplayLifecycleStatus::kCancelled,
             "pre-start cancellation publishes final event")) return false;
  if (!Check(h.summary_store.last.has_value() && h.summary_store.last->partial,
             "pre-start cancellation saves partial summary")) return false;
  return true;
}

bool test_cancelled_before_running_transition_is_not_started() {
  Harness h;
  h.CreateSession("sess-cancel-race");
  h.session_repo.cancel_on_get_call = 2;
  SeedBatches(h, 2);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 99.0),
      MakeExec(BatchOutcome::kOk, 99.0),
  };

  RunReplaySession uc(h.Deps(), FixedClock());
  auto result = uc.Run(MakeReq("sess-cancel-race", 2));

  if (!Check(result.final_status == ReplaySessionStatus::kCancelled,
             "race cancellation -> cancelled")) return false;
  if (!Check(result.summary.total_batches == 2,
             "race cancellation uses loaded total batches")) return false;
  if (!Check(result.summary.processed_batches == 0,
             "race cancellation has no processed batches")) return false;
  if (!Check(result.error_details == "cancelled during preparation",
             "race cancellation propagates cancel details")) return false;
  if (!Check(h.executor.seen_batch_id.empty(),
             "race cancellation must not execute batches")) return false;
  for (const auto& [_, patch] : h.session_repo.patches) {
    if (patch.status.has_value() && *patch.status == ReplaySessionStatus::kRunning) {
      return Check(false, "race cancellation must not patch running");
    }
  }
  if (!Check(!h.publisher.lifecycle.empty() &&
                 h.publisher.lifecycle.back().status == ReplayLifecycleStatus::kCancelled,
             "race cancellation publishes final event")) return false;
  if (!Check(h.summary_store.last.has_value() &&
                 h.summary_store.last->total_batches == 2 &&
                 h.summary_store.last->partial,
             "race cancellation saves partial summary with total")) return false;
  return true;
}

bool test_missing_history_fails_hard() {
  Harness h;
  h.CreateSession("sess-empty");  // <-- СОЗДАЕМ СЕССИЮ
  RunReplaySession uc(h.Deps(), FixedClock());
  auto result = uc.Run(MakeReq("sess-empty", 0));
  if (!Check(result.final_status == ReplaySessionStatus::kFailed,
             "missing history fails")) return false;
  if (!Check(Contains(result.error_details, "no_data"),
             "no_data code returned"))
    return false;
  if (!Check(result.summary.processed_batches == 0, "0 processed")) return false;
  if (!Check(result.summary.total_batches == 0, "0 total")) return false;
  if (!Check(result.summary.partial, "missing history is partial failure")) return false;
  if (!Check(h.summary_store.last.has_value(), "partial summary saved for missing history"))
    return false;
  if (!Check(h.summary_store.last->partial, "saved missing-history summary is partial"))
    return false;
  if (!Check(!h.publisher.lifecycle.empty() &&
                 h.publisher.lifecycle.back().summary.has_value(),
             "failed lifecycle carries partial summary"))
    return false;
  if (!Check(h.executor.seen_batch_id.empty(), "executor not invoked")) return false;
  return true;
}

bool test_incomplete_history_is_soft_failure() {
  Harness h;
  h.CreateSession("sess-history-gap");
  SeedBatches(h, 2);
  h.history.batches[0].fills_count = 2;
  h.history.fills.push_back(MakeFill("b0", 100, "ord-1"));

  RunReplaySession uc(h.Deps(), FixedClock());
  auto result = uc.Run(MakeReq("sess-history-gap", 2));

  if (!Check(result.final_status == ReplaySessionStatus::kCompleted,
             "incomplete history is skipped and replay completes"))
    return false;
  if (!Check(result.error_details.empty(), "soft history gap does not become session error"))
    return false;
  if (!Check(result.summary.processed_batches == 1, "valid batches still processed"))
    return false;
  if (!Check(result.summary.failed_batches == 1, "1 failed batch counted")) return false;
  if (!Check(result.summary.partial, "summary partial on incomplete history")) return false;
  if (!Check(h.executor.seen_batch_id.size() == 1 &&
                 h.executor.seen_batch_id[0] == "b1",
             "executor skips corrupt batch and runs later batch"))
    return false;
  if (!Check(h.agent_log.entries.size() == 2, "soft history failure logged")) return false;
  if (!Check(h.agent_log.entries.front().failure_component == FailureComponent::kHistory,
             "history failure component logged"))
    return false;
  if (!Check(h.agent_log.entries.front().error_code == "history_incomplete",
             "history_incomplete code logged on corrupt batch"))
    return false;
  return true;
}

bool test_config_failure_marks_failed_without_running() {
  Harness h;
  h.CreateSession("sess-cfg");  // <-- СОЗДАЕМ СЕССИЮ
  SeedBatches(h, 1);
  RunReplaySession::Request req = MakeReq("sess-cfg", 1);
  req.config_request.solver_config_id.clear();
  req.config_request.solver_config_inline_override.reset();

  RunReplaySession uc(h.Deps(), FixedClock());
  auto result = uc.Run(req);

  if (!Check(result.final_status == ReplaySessionStatus::kFailed, "failed early")) return false;
  if (!Check(Contains(result.error_details, "config_snapshot_failed"),
             "config code propagated"))
    return false;
  if (!Check(Contains(result.error_details, "Config snapshot"),
             "config error propagated"))
    return false;
  if (!Check(h.summary_store.last.has_value(), "partial summary saved for config failure"))
    return false;
  if (!Check(h.summary_store.last->partial, "saved config failure summary is partial"))
    return false;
  if (!Check(h.executor.seen_batch_id.empty(), "executor never called")) return false;
  return true;
}

bool test_shadow_namespace_passed_to_executor() {
  Harness h;
  h.CreateSession("sess-ns");  // <-- СОЗДАЕМ СЕССИЮ
  SeedBatches(h, 1);
  h.executor.scripted = {MakeExec(BatchOutcome::kOk)};

  RunReplaySession uc(h.Deps(), FixedClock());
  uc.Run(MakeReq("sess-ns", 1));

  if (!Check(h.executor.seen_namespace.size() == 1, "executor called once")) return false;
  if (!Check(h.executor.seen_namespace[0] == "replay::sess-ns",
             "executor receives derived namespace id: " + h.executor.seen_namespace[0]))
    return false;
  if (!Check(h.shadow_ledger.NamespaceCount() == 1, "shadow namespace created")) return false;
  return true;
}

bool test_missing_dependencies_fails_safely() {
  RunReplaySession::Dependencies deps;  // all null
  RunReplaySession uc(deps, FixedClock());
  auto result = uc.Run(MakeReq("sess-x", 0));
  if (!Check(result.final_status == ReplaySessionStatus::kFailed, "failed")) return false;
  if (!Check(Contains(result.error_details, "dependency_error"),
             "dependency code returned: " + result.error_details))
    return false;
  return Check(Contains(result.error_details, "dependencies"),
               "error mentions missing deps: " + result.error_details);
}

bool test_permissions_denied_fails_before_execution() {
  Harness h;
  h.CreateSession("sess-rbac");
  SeedBatches(h, 1);
  h.executor.scripted = {MakeExec(BatchOutcome::kOk, 1.0)};

  auto deps = h.Deps();
  cex::backtest::app::RbacEngine rbac(std::shared_ptr<cex::backtest::app::PostgresRbacRepository>{});
  deps.rbac_engine = &rbac;

  RunReplaySession uc(deps, FixedClock());
  const auto result = uc.Run(MakeReq("sess-rbac", 1));

  if (!Check(result.final_status == ReplaySessionStatus::kFailed,
             "rbac deny -> failed")) return false;
  if (!Check(Contains(result.error_details, "unauthorized"),
             "error must include unauthorized code")) return false;
  if (!Check(h.executor.seen_batch_id.empty(), "executor must not run when denied")) return false;
  return true;
}

bool test_progress_publisher_emits_per_batch() {
  Harness h;
  h.CreateSession("sess-prog");  // <-- СОЗДАЕМ СЕССИЮ
  SeedBatches(h, 3);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 1),
      MakeExec(BatchOutcome::kOk, 2),
      MakeExec(BatchOutcome::kOk, 3),
  };
  RunReplaySession uc(h.Deps(), FixedClock());
  uc.Run(MakeReq("sess-prog", 3));

  if (!Check(h.publisher.progress.size() == 3, "3 progress events")) return false;
  // Sequence: 1..N
  for (size_t i = 0; i < 3; ++i) {
    if (!Check(h.publisher.progress[i].batch_seq == static_cast<uint32_t>(i + 1),
               "progress batch_seq monotonic"))
      return false;
    if (!Check(h.publisher.progress[i].total_batches == 3, "total carried")) return false;
  }
  return true;
}

bool test_duplicate_batch_id_replaces_step_without_duplication() {
  Harness h;

  HistoricalBatchResultRow first;
  first.batch_id = "dup-batch";
  first.event_time_ms = 100;
  h.history.batches.push_back(first);

  HistoricalBatchResultRow second;
  second.batch_id = "dup-batch";
  second.event_time_ms = 200;
  h.history.batches.push_back(second);

  h.executor.scripted = {
      MakeExec(BatchOutcome::kOk, 10.0, 0.2, 0.5, 1, 7),
      MakeExec(BatchOutcome::kOk, 42.0, 0.4, 0.8, 2, 9),
  };

  h.CreateSession("sess-dup");

  RunReplaySession uc(h.Deps(), FixedClock());
  auto result = uc.Run(MakeReq("sess-dup", 2));

  if (!Check(result.final_status == ReplaySessionStatus::kCompleted, "completed")) return false;
  if (!Check(result.summary.total_batches == 1, "duplicate total counted once"))
    return false;
  if (!Check(result.summary.processed_batches == 1, "duplicate batch counted once"))
    return false;
  if (!Check(result.summary.failed_batches == 0, "no failed batches")) return false;
  if (!Check(!result.summary.partial, "duplicate replacement is complete"))
    return false;
  if (!Check(std::abs(result.summary.total_pnl - 42.0) < 1e-9,
             "summary uses rerun replacement result"))
    return false;
  if (!Check(h.agent_log.entries.size() == 1, "agent log upserted once")) return false;
  if (!Check(std::abs(h.agent_log.entries.front().pnl - 42.0) < 1e-9,
             "agent log replaced with rerun payload"))
    return false;
  if (!Check(h.publisher.progress.size() == 2, "progress emitted for both attempts"))
    return false;
  for (const auto& evt : h.publisher.progress) {
    if (!Check(evt.total_batches == 1, "duplicate progress total counted once"))
      return false;
  }
  return true;
}

bool test_runtime_metrics_wired_into_run() {
  Harness h;
  h.CreateSession("sess-metrics");  // <-- СОЗДАЕМ СЕССИЮ
  SeedBatches(h, 2);
  h.executor.scripted = {
      MakeExec(BatchOutcome::kSoftFailure,
               0,
               0,
               0,
               0,
               7,
               "solver diverged",
               "solver_diverged",
               FailureComponent::kSolver),
      MakeExec(BatchOutcome::kHardFailure,
               0,
               0,
               0,
               0,
               12,
               "ledger broke",
               "ledger_apply_failed",
               FailureComponent::kLedger),
  };

  RunReplaySession uc(h.Deps(), FixedClock());
  uc.Run(MakeReq("sess-metrics", 2));

  const auto rendered = h.runtime_metrics.RenderPrometheus();
  if (!Check(Contains(rendered, "backtest_replay_history_load_latency_ms_bucket"),
             "history latency exported")) return false;
  if (!Check(Contains(rendered, "backtest_replay_failures_total{failure_type=\"soft\",component=\"solver\"} 1"),
             "soft solver failure exported")) return false;
  if (!Check(Contains(rendered, "backtest_replay_failures_total{failure_type=\"hard\",component=\"ledger\"} 1"),
             "hard ledger failure exported")) return false;
  if (!Check(Contains(rendered, "backtest_replay_shadow_namespace_init_total{result=\"created\"} 1"),
             "shadow init exported")) return false;
  return true;
}

}  // namespace

int main() {
  bool all_passed = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) {
      std::cerr << "  in test: " << name << std::endl;
      all_passed = false;
    }
  };

  run("test_completed_happy_path", test_completed_happy_path);
  run("test_status_transitions_pending_running_completed",
      test_status_transitions_pending_running_completed);
  run("test_hard_failure_marks_failed_and_partial",
      test_hard_failure_marks_failed_and_partial);
  run("test_agent_log_write_failure_marks_failed_and_partial",
      test_agent_log_write_failure_marks_failed_and_partial);
  run("test_soft_failures_continue_session", test_soft_failures_continue_session);
  run("test_reward_policy_incremental_pnl_with_failures_zeroed",
      test_reward_policy_incremental_pnl_with_failures_zeroed);
  run("test_reward_policy_negative_is_from_snapshot",
      test_reward_policy_negative_is_from_snapshot);
  run("test_unsupported_reward_policy_fails_before_batches",
      test_unsupported_reward_policy_fails_before_batches);
  run("test_materialized_snapshot_passed_to_executor",
      test_materialized_snapshot_passed_to_executor);
  run("test_session_strategy_passed_to_executor",
      test_session_strategy_passed_to_executor);
  run("test_persisted_session_snapshot_reused_for_replay",
      test_persisted_session_snapshot_reused_for_replay);
  run("test_executor_payloads_are_persisted_to_agent_log",
      test_executor_payloads_are_persisted_to_agent_log);
  run("test_cancellation_stops_loop", test_cancellation_stops_loop);
  run("test_terminal_session_is_not_restarted",
      test_terminal_session_is_not_restarted);
  run("test_cancel_token_before_start_does_not_promote_to_running",
      test_cancel_token_before_start_does_not_promote_to_running);
  run("test_cancelled_before_running_transition_is_not_started",
      test_cancelled_before_running_transition_is_not_started);
  run("test_missing_history_fails_hard", test_missing_history_fails_hard);
  run("test_incomplete_history_is_soft_failure",
      test_incomplete_history_is_soft_failure);
  run("test_config_failure_marks_failed_without_running",
      test_config_failure_marks_failed_without_running);
  run("test_shadow_namespace_passed_to_executor", test_shadow_namespace_passed_to_executor);
  run("test_missing_dependencies_fails_safely", test_missing_dependencies_fails_safely);
  run("test_permissions_denied_fails_before_execution",
      test_permissions_denied_fails_before_execution);
  run("test_progress_publisher_emits_per_batch", test_progress_publisher_emits_per_batch);
  run("test_duplicate_batch_id_replaces_step_without_duplication",
      test_duplicate_batch_id_replaces_step_without_duplication);
  run("test_runtime_metrics_wired_into_run", test_runtime_metrics_wired_into_run);

  if (all_passed) {
    std::cout << "[OK] backtest_run_replay_session_test passed" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
