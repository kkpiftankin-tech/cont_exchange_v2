#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/historical_batch_loader_uc.hpp"
#include "app/replay_config_snapshot.hpp"
#include "app/replay_orchestration_ports.hpp"
#include "app/replay_runtime_metrics.hpp"
#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"
#include "app/run_replay_session_uc.hpp"
#include "app/shadow_namespace_uc.hpp"
#include "cex/common/kafka.hpp"
#include "cex/common/replay_kafka.hpp"
#include "infra/clickhouse_storage.hpp"
#include "infra/in_memory_shadow_ledger.hpp"

namespace {

using cex::backtest::app::AgentLogEntry;
using cex::backtest::app::BatchExecutionResult;
using cex::backtest::app::BatchOutcome;
using cex::backtest::app::FailureComponent;
using cex::backtest::app::HistoricalBatch;
using cex::backtest::app::HistoricalBatchLoaderUseCases;
using cex::backtest::app::HistoricalBatchResultRow;
using cex::backtest::app::HistoricalFillRow;
using cex::backtest::app::HistoricalMarketdataSnapshotRow;
using cex::backtest::app::IAgentLogWriter;
using cex::backtest::app::IBatchExecutor;
using cex::backtest::app::IHistoricalBatchLoader;
using cex::backtest::app::IReplayEventPublisher;
using cex::backtest::app::IReplaySummaryStore;
using cex::backtest::app::ReplayConfigRequest;
using cex::backtest::app::ReplayLifecycleEvent;
using cex::backtest::app::ReplayLifecycleStatus;
using cex::backtest::app::ReplayProgressEvent;
using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionListFilter;
using cex::backtest::app::ReplaySessionRepositoryPort;
using cex::backtest::app::ReplaySessionStatePatch;
using cex::backtest::app::ReplaySessionStatus;
using cex::backtest::app::ReplaySummary;
using cex::backtest::app::RunReplaySession;
using cex::backtest::app::ShadowLedgerApplyRequest;
using cex::backtest::app::ShadowLedgerFill;
using cex::backtest::app::ShadowNamespaceInitializer;


int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string GetEnv(const char* key, const std::string& fallback) {
  const char* value = std::getenv(key);
  return value == nullptr || std::string(value).empty() ? fallback : std::string(value);
}

std::string StatusName(ReplaySessionStatus status) {
  switch (status) {
    case ReplaySessionStatus::kPending: return "pending";
    case ReplaySessionStatus::kRunning: return "running";
    case ReplaySessionStatus::kCompleted: return "completed";
    case ReplaySessionStatus::kFailed: return "failed";
    case ReplaySessionStatus::kCancelled: return "cancelled";
  }
  return "unknown";
}

std::string LifecycleStatusName(ReplayLifecycleStatus status) {
  switch (status) {
    case ReplayLifecycleStatus::kRunning: return "running";
    case ReplayLifecycleStatus::kCompleted: return "completed";
    case ReplayLifecycleStatus::kFailed: return "failed";
    case ReplayLifecycleStatus::kCancelled: return "cancelled";
  }
  return "unknown";
}

double Percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const double idx = (static_cast<double>(values.size() - 1) * p);
  const std::size_t lo = static_cast<std::size_t>(std::floor(idx));
  const std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
  if (lo == hi) return values[lo];
  const double w = idx - static_cast<double>(lo);
  return values[lo] * (1.0 - w) + values[hi] * w;
}

double SecondsSince(std::chrono::steady_clock::time_point started) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

std::string Fixed(double value, int precision = 3) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

double ParseDouble(const std::string& value) {
  try {
    return std::stod(value);
  } catch (...) {
    return 0.0;
  }
}

struct Options {
  std::string clickhouse_url{"http://clickhouse:8123"};
  std::string kafka_brokers{"redpanda:9092"};
  std::string clickhouse_db{"backtest"};
  std::string kafka_topic{"replay.results"};
  std::string report_path{"artifacts/f15_test3_load_report.md"};
  uint32_t single_batches{1000};
  uint32_t large_batches{10000};
  uint32_t parallel_batches{1000};
};

Options ParseOptions(int argc, char** argv) {
  Options options;
  options.clickhouse_url = GetEnv("F15_LOAD_CLICKHOUSE_URL", options.clickhouse_url);
  options.kafka_brokers = GetEnv("F15_LOAD_KAFKA_BROKERS", options.kafka_brokers);
  options.clickhouse_db = GetEnv("F15_LOAD_CLICKHOUSE_DB", options.clickhouse_db);
  options.kafka_topic = GetEnv("F15_LOAD_KAFKA_TOPIC", options.kafka_topic);
  options.report_path = GetEnv("F15_LOAD_REPORT", options.report_path);

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto apply_value = [&](const std::string& key, std::string* out) {
      const std::string prefix = "--" + key + "=";
      if (arg.rfind(prefix, 0) == 0) {
        *out = arg.substr(prefix.size());
        return true;
      }
      return false;
    };
    auto apply_u32 = [&](const std::string& key, uint32_t* out) {
      std::string text;
      if (!apply_value(key, &text)) return false;
      *out = static_cast<uint32_t>(std::max<long>(1, std::strtol(text.c_str(), nullptr, 10)));
      return true;
    };
    if (apply_value("clickhouse-url", &options.clickhouse_url)) continue;
    if (apply_value("kafka-brokers", &options.kafka_brokers)) continue;
    if (apply_value("clickhouse-db", &options.clickhouse_db)) continue;
    if (apply_value("kafka-topic", &options.kafka_topic)) continue;
    if (apply_value("report", &options.report_path)) continue;
    if (apply_u32("single-batches", &options.single_batches)) continue;
    if (apply_u32("large-batches", &options.large_batches)) continue;
    if (apply_u32("parallel-batches", &options.parallel_batches)) continue;
  }
  return options;
}

class SyntheticHistoricalLoader final : public IHistoricalBatchLoader {
 public:
  std::vector<HistoricalBatchResultRow> LoadBatchResults(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    std::vector<HistoricalBatchResultRow> rows;
    const int64_t count = std::max<int64_t>(0, to_ms - from_ms + 1);
    if (offset >= count || limit <= 0) return rows;
    const int64_t end = std::min(count, offset + limit);
    rows.reserve(static_cast<std::size_t>(end - offset));
    for (int64_t i = offset; i < end; ++i) {
      HistoricalBatchResultRow row;
      row.batch_id = BatchId(from_ms, i);
      row.event_time_ms = from_ms + i;
      row.source = "f15-load";
      row.correlation_id = "f15-load";
      row.partition_key = "BTC/USDT";
      row.residual_norm = 0.000001 * static_cast<double>((i % 7) + 1);
      row.solve_time_ms = 1;
      row.num_active_orders = 2;
      row.config_version = 1;
      row.solver_diagnostics_json = "{}";
      row.clear_prices_json = "{\"BTC/USDT\":68000}";
      row.executed_rates_json = "{}";
      row.used_liquidity_json = "{}";
      row.fills_count = 1;
      rows.push_back(std::move(row));
    }
    return rows;
  }

  std::vector<HistoricalFillRow> LoadFills(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    std::vector<HistoricalFillRow> rows;
    const int64_t count = std::max<int64_t>(0, to_ms - from_ms + 1);
    if (offset >= count || limit <= 0) return rows;
    const int64_t end = std::min(count, offset + limit);
    rows.reserve(static_cast<std::size_t>(end - offset));
    for (int64_t i = offset; i < end; ++i) {
      const double price = 68000.0 + static_cast<double>(i % 100);
      const double qty = 0.001 + static_cast<double>(i % 5) * 0.00001;
      HistoricalFillRow row;
      row.batch_id = BatchId(from_ms, i);
      row.event_time_ms = from_ms + i;
      row.order_id = "order-" + std::to_string(from_ms) + "-" + std::to_string(i);
      row.user_id = "f15-load-user";
      row.symbol = "BTC/USDT";
      row.base = "BTC";
      row.quote = "USDT";
      row.side = (i % 2 == 0) ? "buy" : "sell";
      row.executed_qty = qty;
      row.price = price;
      row.executed_notional = qty * price;
      row.fee_amount = row.executed_notional * 0.0002;
      row.fee_currency = "USDT";
      row.liquidity_source = "synthetic";
      row.venue_id = "binance";
      row.snapshot_id = "snapshot-" + std::to_string(i);
      row.curve_id = "curve-" + std::to_string(i);
      rows.push_back(std::move(row));
    }
    return rows;
  }

  std::vector<HistoricalMarketdataSnapshotRow> LoadMarketdataSnapshots(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    std::vector<HistoricalMarketdataSnapshotRow> rows;
    const int64_t count = std::max<int64_t>(0, to_ms - from_ms + 1);
    if (offset >= count || limit <= 0) return rows;
    const int64_t end = std::min(count, offset + limit);
    rows.reserve(static_cast<std::size_t>(end - offset));
    for (int64_t i = offset; i < end; ++i) {
      const double mid = 68000.0 + static_cast<double>(i % 100);
      HistoricalMarketdataSnapshotRow row;
      row.venue_id = "binance";
      row.symbol = "BTC/USDT";
      row.event_time_ms = from_ms + i;
      row.best_bid = mid - 0.5;
      row.best_ask = mid + 0.5;
      row.mid_price = mid;
      row.spread = 1.0;
      row.bid_depth_json = "[]";
      row.ask_depth_json = "[]";
      row.tick_size = 0.01;
      row.lot_size = 0.00001;
      row.status = "ok";
      rows.push_back(std::move(row));
    }
    return rows;
  }

 private:
  static std::string BatchId(int64_t from_ms, int64_t i) {
    return "f15-load-batch-" + std::to_string(from_ms) + "-" + std::to_string(i);
  }
};

class InMemorySessionRepo final : public ReplaySessionRepositoryPort {
 public:
  ReplaySession Create(const ReplaySession& session) override {
    std::lock_guard<std::mutex> lock(mu_);
    sessions_[session.session_id] = session;
    return session;
  }

  std::optional<ReplaySession> GetById(const std::string& session_id) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return std::nullopt;
    return it->second;
  }

  std::vector<ReplaySession> List(const ReplaySessionListFilter& filter) override {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ReplaySession> out;
    for (const auto& [_, session] : sessions_) {
      if (filter.user_id.has_value() && session.user_id != *filter.user_id) continue;
      if (filter.status.has_value() && session.status != *filter.status) continue;
      out.push_back(session);
    }
    return out;
  }

  bool UpdateState(const std::string& session_id,
                   const ReplaySessionStatePatch& patch) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    if (patch.status.has_value()) it->second.status = *patch.status;
    if (patch.total_batches.has_value()) it->second.total_batches = *patch.total_batches;
    if (patch.progress_batches.has_value()) it->second.progress_batches = *patch.progress_batches;
    if (patch.started_at.has_value()) it->second.started_at = *patch.started_at;
    if (patch.completed_at.has_value()) it->second.completed_at = *patch.completed_at;
    if (patch.error_details.has_value()) it->second.error_details = *patch.error_details;
    return true;
  }

  std::vector<ReplaySession> GetRetryChain(const std::string& session_id) override {
    auto session = GetById(session_id);
    if (!session.has_value()) return {};
    return {*session};
  }

 private:
  std::mutex mu_;
  std::map<std::string, ReplaySession> sessions_;
};

class SummaryStore final : public IReplaySummaryStore {
 public:
  void SaveSummary(const ReplaySummary& summary) override {
    std::lock_guard<std::mutex> lock(mu_);
    summaries_[summary.session_id] = summary;
  }

  std::optional<ReplaySummary> Get(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = summaries_.find(session_id);
    if (it == summaries_.end()) return std::nullopt;
    return it->second;
  }

 private:
  mutable std::mutex mu_;
  std::map<std::string, ReplaySummary> summaries_;
};

class TimedAgentLogWriter final : public IAgentLogWriter {
 public:
  explicit TimedAgentLogWriter(IAgentLogWriter* inner) : inner_(inner) {}

  void WriteAgentLog(const AgentLogEntry& entry) override {
    const auto started = std::chrono::steady_clock::now();
    if (inner_ != nullptr) inner_->WriteAgentLog(entry);
    const auto latency_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    std::lock_guard<std::mutex> lock(mu_);
    latencies_ms_.push_back(latency_ms);
  }

  std::vector<double> LatenciesMs() const {
    std::lock_guard<std::mutex> lock(mu_);
    return latencies_ms_;
  }

 private:
  IAgentLogWriter* inner_{nullptr};
  mutable std::mutex mu_;
  std::vector<double> latencies_ms_;
};

class ShadowBatchExecutor final : public IBatchExecutor {
 public:
  explicit ShadowBatchExecutor(cex::backtest::app::IShadowLedger* ledger)
      : ledger_(ledger) {}

  BatchExecutionResult ExecuteBatch(const std::string& namespace_id,
                                    const std::string& session_config_snapshot_json,
                                    const std::string& strategy_json,
                                    const std::string& tracked_user_id,
                                    const std::string& reporting_currency,
                                    const HistoricalBatch& batch) override {
    (void)session_config_snapshot_json;
    (void)strategy_json;
    (void)tracked_user_id;
    (void)reporting_currency;
    const auto started = std::chrono::steady_clock::now();

    ShadowLedgerApplyRequest request;
    request.namespace_id = namespace_id;
    request.batch_id = batch.batch_result.batch_id;
    request.tracked_user_id = "f15-load-user";
    request.reporting_currency = "USDT";

    for (const auto& fill : batch.fills) {
      ShadowLedgerFill shadow_fill;
      shadow_fill.order_id = fill.order_id;
      shadow_fill.user_id = fill.user_id;
      shadow_fill.symbol = fill.symbol;
      shadow_fill.base = fill.base;
      shadow_fill.quote = fill.quote;
      shadow_fill.side = fill.side;
      shadow_fill.executed_qty = fill.executed_qty;
      shadow_fill.price = fill.price;
      shadow_fill.executed_notional = fill.executed_notional;
      shadow_fill.fee_amount = fill.fee_amount;
      shadow_fill.fee_currency = fill.fee_currency;
      shadow_fill.liquidity_source = fill.liquidity_source;
      shadow_fill.venue_id = fill.venue_id;
      shadow_fill.snapshot_id = fill.snapshot_id;
      shadow_fill.curve_id = fill.curve_id;
      request.fills.push_back(std::move(shadow_fill));
      request.clear_prices[fill.symbol] = fill.price;
    }
    if (request.clear_prices.empty()) {
      request.clear_prices["BTC/USDT"] = 68000.0;
    }

    BatchExecutionResult result;
    if (ledger_ == nullptr) {
      result.outcome = BatchOutcome::kHardFailure;
      result.failure_component = FailureComponent::kLedger;
      result.error_code = "shadow_ledger_missing";
      result.error_details = "shadow ledger dependency is missing";
      return result;
    }

    const auto step = ledger_->ApplyFills(request);
    const auto elapsed_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    {
      std::lock_guard<std::mutex> lock(mu_);
      solve_latencies_ms_.push_back(elapsed_ms);
    }

    if (!step.ok) {
      result.outcome = BatchOutcome::kHardFailure;
      result.failure_component = FailureComponent::kLedger;
      result.error_code = step.error_code.empty() ? "shadow_apply_failed" : step.error_code;
      result.error_details = step.error_message;
      ledger_errors_.fetch_add(1);
      return result;
    }

    result.outcome = BatchOutcome::kOk;
    result.pnl = ParseDouble(step.total_pnl);
    result.is_value = 0.0;
    result.fill_rate = batch.batch_result.fills_count == 0
                           ? 0.0
                           : static_cast<double>(request.fills.size()) /
                                 static_cast<double>(batch.batch_result.fills_count);
    result.fills_applied = static_cast<uint32_t>(request.fills.size());
    result.solve_time_ms = static_cast<uint32_t>(std::max(1.0, std::round(elapsed_ms)));
    result.residual_norm = batch.batch_result.residual_norm;
    result.vwap = request.fills.empty() ? 0.0 : request.fills.front().price;
    return result;
  }

  std::vector<double> SolveLatenciesMs() const {
    std::lock_guard<std::mutex> lock(mu_);
    return solve_latencies_ms_;
  }

  uint64_t LedgerErrors() const { return ledger_errors_.load(); }

 private:
  cex::backtest::app::IShadowLedger* ledger_{nullptr};
  mutable std::mutex mu_;
  std::vector<double> solve_latencies_ms_;
  std::atomic<uint64_t> ledger_errors_{0};
};

class QuietKafkaPublisher final : public IReplayEventPublisher {
 public:
  QuietKafkaPublisher(std::string brokers,
                      std::string topic,
                      cex::backtest::app::ReplayRuntimeMetrics* metrics)
      : producer_({.brokers = std::move(brokers), .client_id = "f15-load-publisher"}),
        topic_(std::move(topic)),
        metrics_(metrics) {}

  void PublishProgress(const ReplayProgressEvent& evt) override {
    cex::common::ReplayResultMessage msg;
    msg.kind = cex::common::ReplayResultKind::kProgress;
    msg.session_id = evt.session_id;
    msg.published_at_ms = NowMs();
    msg.batch_seq = evt.batch_seq;
    msg.total_batches = evt.total_batches;
    Publish(evt.session_id, msg);
  }

  void PublishLifecycle(const ReplayLifecycleEvent& evt) override {
    cex::common::ReplayResultMessage msg;
    msg.kind = cex::common::ReplayResultKind::kLifecycle;
    msg.session_id = evt.session_id;
    msg.published_at_ms = NowMs();
    msg.status = LifecycleStatusName(evt.status);
    if (evt.summary.has_value()) {
      msg.has_summary = true;
      msg.total_batches = evt.summary->total_batches;
      msg.processed_batches = evt.summary->processed_batches;
      msg.failed_batches = evt.summary->failed_batches;
      msg.partial = evt.summary->partial;
      msg.total_pnl = evt.summary->total_pnl;
      msg.avg_pnl = evt.summary->avg_pnl;
      msg.avg_is = evt.summary->avg_is;
      msg.sharpe = evt.summary->sharpe;
      msg.avg_fill_rate = evt.summary->avg_fill_rate;
      msg.avg_solve_time_ms = evt.summary->avg_solve_time_ms;
      msg.max_drawdown = evt.summary->max_drawdown;
      msg.std_pnl = evt.summary->std_pnl;
      msg.avg_vwap = evt.summary->avg_vwap;
    }
    if (evt.error_details.has_value()) msg.error_details = *evt.error_details;
    if (evt.error_code.has_value()) msg.error_code = *evt.error_code;
    Publish(evt.session_id, msg);
  }

 private:
  void Publish(const std::string& key, const cex::common::ReplayResultMessage& msg) {
    const auto started = std::chrono::steady_clock::now();
    const bool ok = producer_.produce(topic_, key, cex::common::SerializeReplayResultMessage(msg));
    if (metrics_ != nullptr) {
      const auto latency_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
      metrics_->ObserveKafkaPublish(topic_, latency_ms, ok);
    }
  }

  cex::common::KafkaProducer producer_;
  std::string topic_;
  cex::backtest::app::ReplayRuntimeMetrics* metrics_{nullptr};
};

class KafkaProbe {
 public:
  KafkaProbe(std::string brokers, std::string topic, std::string group_id, std::string prefix)
      : brokers_(std::move(brokers)),
        topic_(std::move(topic)),
        group_id_(std::move(group_id)),
        prefix_(std::move(prefix)) {}

  void Start() {
    running_.store(true);
    thread_ = std::thread([this] { Loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  void WaitFor(uint64_t expected, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (Consumed() >= expected) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void Stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
  }

  uint64_t Consumed() const { return consumed_.load(); }

  std::vector<double> LagLatenciesMs() const {
    std::lock_guard<std::mutex> lock(mu_);
    return lag_ms_;
  }

 private:
  void Loop() {
    cex::common::KafkaConsumer consumer({
        .brokers = brokers_,
        .group_id = group_id_,
        .client_id = group_id_,
        .enable_auto_commit = false,
        .auto_offset_reset = "latest",
        .max_poll_interval_ms = 600000,
    });
    if (!consumer.subscribe({topic_})) return;
    while (running_.load()) {
      const bool ok = consumer.poll_once(
          100, [this](const std::string& topic, const std::string& key,
                      const std::string& payload) {
            (void)topic;
            (void)key;
            cex::common::ReplayResultMessage msg;
            if (!cex::common::ParseReplayResultMessage(payload, &msg)) return;
            if (!StartsWith(msg.session_id, prefix_)) return;
            consumed_.fetch_add(1);
            const int64_t lag = std::max<int64_t>(0, NowMs() - msg.published_at_ms);
            std::lock_guard<std::mutex> lock(mu_);
            lag_ms_.push_back(static_cast<double>(lag));
          });
      if (!ok) break;
    }
  }

  std::string brokers_;
  std::string topic_;
  std::string group_id_;
  std::string prefix_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> consumed_{0};
  mutable std::mutex mu_;
  std::vector<double> lag_ms_;
  std::thread thread_;
};

struct Scenario {
  std::string name;
  uint32_t sessions{1};
  uint32_t batches_per_session{1000};
};

struct ScenarioResult {
  std::string name;
  uint32_t sessions{0};
  uint32_t batches_per_session{0};
  uint64_t expected_batches{0};
  uint64_t expected_kafka_messages{0};
  uint64_t clickhouse_rows{0};
  uint64_t kafka_consumed{0};
  uint64_t ledger_errors{0};
  std::size_t shadow_namespaces_active{0};
  double duration_sec{0.0};
  double batches_per_sec{0.0};
  double clickhouse_rows_per_sec{0.0};
  double ch_insert_p50_ms{0.0};
  double ch_insert_p95_ms{0.0};
  double solve_p50_ms{0.0};
  double solve_p95_ms{0.0};
  double kafka_lag_p50_ms{0.0};
  double kafka_lag_p95_ms{0.0};
  uint64_t kafka_lag_messages{0};
  bool all_completed{false};
};

ReplaySession MakeSession(const std::string& session_id, uint32_t batches) {
  ReplaySession s;
  s.session_id = session_id;
  s.user_id = "f15-load-user";
  s.name = "F15 load " + session_id;
  s.strategy_json = "{\"type\":\"synthetic-load\"}";
  s.date_range_from = std::chrono::system_clock::now();
  s.date_range_to = s.date_range_from + std::chrono::milliseconds(batches);
  s.solver_config_id = "inline";
  s.risk_limits_id = "inline";
  s.fee_model_json = "{\"type\":\"flat\"}";
  s.status = ReplaySessionStatus::kPending;
  s.progress_batches = 0;
  s.created_at = std::chrono::system_clock::now();
  return s;
}

ReplayConfigRequest MakeConfigRequest() {
  ReplayConfigRequest request;
  request.solver_config_id = "inline";
  request.risk_limits_id = "inline";
  request.fee_model_id = "inline";
  request.reward_config_id = "inline";
  request.solver_config_inline_override = "{\"solver\":\"synthetic\"}";
  request.risk_limits_inline_override = "{\"max_notional\":1000000000}";
  request.fee_model_inline_override = "{\"maker_bps\":2,\"taker_bps\":2}";
  request.reward_config_inline_override = "{\"mode\":\"incrementalPnL\"}";
  request.random_seed = 15;
  request.tolerance = 1e-9;
  return request;
}

uint64_t CountRows(cex::backtest::infra::ClickHouseReplayStorage* storage,
                   const std::vector<std::string>& session_ids) {
  uint64_t total = 0;
  for (const auto& session_id : session_ids) {
    total += storage->LoadAgentLogRefsBySessionId(session_id).size();
  }
  return total;
}

ScenarioResult RunScenario(
    const Scenario& scenario,
    const std::string& run_id,
    const Options& options,
    InMemorySessionRepo* session_repo,
    cex::backtest::infra::ClickHouseReplayStorage* clickhouse,
    cex::backtest::infra::InMemoryShadowLedger* shadow_ledger) {
  ScenarioResult result;
  result.name = scenario.name;
  result.sessions = scenario.sessions;
  result.batches_per_session = scenario.batches_per_session;
  result.expected_batches =
      static_cast<uint64_t>(scenario.sessions) * scenario.batches_per_session;
  result.expected_kafka_messages =
      static_cast<uint64_t>(scenario.sessions) *
      (static_cast<uint64_t>(scenario.batches_per_session) + 2);

  const std::string session_prefix = run_id + "-" + scenario.name + "-";
  const int64_t scenario_base_ms = NowMs();
  KafkaProbe probe(options.kafka_brokers, options.kafka_topic,
                   "f15-load-probe-" + run_id + "-" + scenario.name,
                   session_prefix);
  probe.Start();

  cex::backtest::app::ReplayRuntimeMetrics runtime_metrics;
  SyntheticHistoricalLoader synthetic_loader;
  HistoricalBatchLoaderUseCases history_loader(&synthetic_loader);
  cex::backtest::app::ReplayConfigSnapshotBuilder config_builder(nullptr);
  ShadowNamespaceInitializer shadow_init(shadow_ledger);
  ShadowBatchExecutor executor(shadow_ledger);
  TimedAgentLogWriter agent_log(clickhouse);
  SummaryStore summary_store;
  QuietKafkaPublisher publisher(options.kafka_brokers, options.kafka_topic, &runtime_metrics);

  RunReplaySession::Dependencies deps;
  deps.session_repo = session_repo;
  deps.config_builder = &config_builder;
  deps.shadow_init = &shadow_init;
  deps.shadow_ledger = shadow_ledger;
  deps.history_loader = &history_loader;
  deps.batch_executor = &executor;
  deps.agent_log_writer = &agent_log;
  deps.summary_store = &summary_store;
  deps.event_publisher = &publisher;
  deps.runtime_metrics = &runtime_metrics;

  std::atomic<uint32_t> completed{0};
  std::vector<std::string> session_ids;
  std::mutex ids_mu;
  const auto started = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  threads.reserve(scenario.sessions);

  for (uint32_t i = 0; i < scenario.sessions; ++i) {
    const std::string session_id = session_prefix + std::to_string(i);
    {
      std::lock_guard<std::mutex> lock(ids_mu);
      session_ids.push_back(session_id);
    }
    session_repo->Create(MakeSession(session_id, scenario.batches_per_session));

    threads.emplace_back([&, session_id, i] {
      RunReplaySession runner(deps);
      RunReplaySession::Request request;
      request.session_id = session_id;
      request.tracked_user_id = "f15-load-user";
      request.reporting_currency = "USDT";
      request.config_request = MakeConfigRequest();
      const int64_t from = scenario_base_ms +
                           static_cast<int64_t>(i) * 100000000LL +
                           static_cast<int64_t>(scenario.batches_per_session) * 10LL;
      request.history_config.from_ms = from;
      request.history_config.to_ms = from + scenario.batches_per_session - 1;
      request.history_config.chunk_size = 1000;
      const auto run_result = runner.Run(request);
      if (run_result.final_status == ReplaySessionStatus::kCompleted) {
        completed.fetch_add(1);
      }
    });
  }

  for (auto& thread : threads) thread.join();
  result.duration_sec = SecondsSince(started);
  result.batches_per_sec = result.expected_batches / std::max(0.001, result.duration_sec);

  // Let ClickHouse FINAL and Kafka consumer catch up.
  std::this_thread::sleep_for(std::chrono::seconds(2));
  probe.WaitFor(result.expected_kafka_messages, std::chrono::seconds(60));
  result.kafka_consumed = probe.Consumed();
  result.kafka_lag_messages = result.expected_kafka_messages > result.kafka_consumed
                                  ? result.expected_kafka_messages - result.kafka_consumed
                                  : 0;
  probe.Stop();

  result.clickhouse_rows = CountRows(clickhouse, session_ids);
  result.clickhouse_rows_per_sec =
      result.clickhouse_rows / std::max(0.001, result.duration_sec);
  result.ledger_errors = executor.LedgerErrors();
  result.shadow_namespaces_active = shadow_ledger->NamespaceCount();
  result.all_completed = completed.load() == scenario.sessions;

  const auto ch_latencies = agent_log.LatenciesMs();
  result.ch_insert_p50_ms = Percentile(ch_latencies, 0.50);
  result.ch_insert_p95_ms = Percentile(ch_latencies, 0.95);
  const auto solve_latencies = executor.SolveLatenciesMs();
  result.solve_p50_ms = Percentile(solve_latencies, 0.50);
  result.solve_p95_ms = Percentile(solve_latencies, 0.95);
  const auto kafka_lag_latencies = probe.LagLatenciesMs();
  result.kafka_lag_p50_ms = Percentile(kafka_lag_latencies, 0.50);
  result.kafka_lag_p95_ms = Percentile(kafka_lag_latencies, 0.95);
  return result;
}

void WriteReport(const Options& options, const std::vector<ScenarioResult>& results) {
  std::ofstream out(options.report_path);
  out << "# F15-TEST-3 Load Test Report\n\n";
  out << "- ClickHouse: `" << options.clickhouse_url << "` database `" << options.clickhouse_db << "`\n";
  out << "- Kafka brokers: `" << options.kafka_brokers << "`, topic `" << options.kafka_topic << "`\n";
  out << "- Scope: `RunReplaySession` + synthetic historical loader + `InMemoryShadowLedger` + ClickHouse `replay_agentlogs` + Kafka replay events.\n";
  out << "- Note: external Matching/Risk services are not invoked by this harness because the current service wiring does not expose a full replay run endpoint.\n\n";

  out << "| Scenario | Batches | Sessions | Wall, s | Batches/s | CH rows | CH rows/s | CH p95 ms | Kafka consumed/expected | Kafka lag msgs | Kafka lag p95 ms | Solve p95 ms | Shadow namespaces | Ledger errors | Completed |\n";
  out << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
  for (const auto& r : results) {
    out << "| `" << r.name << "`"
        << " | " << r.expected_batches
        << " | " << r.sessions
        << " | " << Fixed(r.duration_sec)
        << " | " << Fixed(r.batches_per_sec)
        << " | " << r.clickhouse_rows
        << " | " << Fixed(r.clickhouse_rows_per_sec)
        << " | " << Fixed(r.ch_insert_p95_ms)
        << " | " << r.kafka_consumed << "/" << r.expected_kafka_messages
        << " | " << r.kafka_lag_messages
        << " | " << Fixed(r.kafka_lag_p95_ms)
        << " | " << Fixed(r.solve_p95_ms)
        << " | " << r.shadow_namespaces_active
        << " | " << r.ledger_errors
        << " | " << (r.all_completed ? "yes" : "no")
        << " |\n";
  }

  auto find = [&](const std::string& name) -> std::optional<ScenarioResult> {
    for (const auto& r : results) {
      if (r.name == name) return r;
    }
    return std::nullopt;
  };
  const auto single = find("single_1000");
  const auto large = find("single_10000");
  const auto parallel5 = find("parallel_5x");
  const auto parallel20 = find("parallel_20x");

  out << "\n## Spec Checks\n\n";
  if (single.has_value()) {
    out << "- 1,000 batches < 60s: "
        << (single->duration_sec < 60.0 ? "PASS" : "FAIL")
        << " (" << Fixed(single->duration_sec) << "s).\n";
    out << "- ClickHouse AgentLog throughput > 200 rows/s on 1,000-batch run: "
        << (single->clickhouse_rows_per_sec > 200.0 ? "PASS" : "FAIL")
        << " (" << Fixed(single->clickhouse_rows_per_sec) << " rows/s).\n";
  }
  if (large.has_value()) {
    out << "- 10,000 batches < 10 min: "
        << (large->duration_sec < 600.0 ? "PASS" : "FAIL")
        << " (" << Fixed(large->duration_sec) << "s).\n";
  }
  if (single.has_value() && parallel5.has_value()) {
    const double degradation = single->solve_p95_ms <= 0.0
                                   ? 0.0
                                   : (parallel5->solve_p95_ms / single->solve_p95_ms - 1.0) * 100.0;
    out << "- 5 parallel sessions solve p95 degradation < 20%: "
        << (degradation < 20.0 ? "PASS" : "FAIL")
        << " (" << Fixed(degradation) << "%).\n";
  }
  if (single.has_value() && parallel20.has_value()) {
    const double ratio = single->solve_p95_ms <= 0.0
                             ? 0.0
                             : parallel20->solve_p95_ms / single->solve_p95_ms;
    out << "- 20 parallel sessions solve p95 <= 2x baseline: "
        << (ratio <= 2.0 ? "PASS" : "FAIL")
        << " (" << Fixed(ratio) << "x).\n";
  }
  const bool kafka_ok = std::all_of(results.begin(), results.end(), [](const ScenarioResult& r) {
    return r.kafka_lag_messages == 0;
  });
  const bool shadow_ok = std::all_of(results.begin(), results.end(), [](const ScenarioResult& r) {
    return r.ledger_errors == 0 && r.all_completed;
  });
  out << "- Kafka lag after catch-up: " << (kafka_ok ? "PASS" : "FAIL") << ".\n";
  out << "- Shadow ledger stability: " << (shadow_ok ? "PASS" : "FAIL")
      << " (no ledger apply errors and all sessions completed).\n";
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = ParseOptions(argc, argv);

  cex::backtest::app::ReplayRuntimeMetrics storage_metrics;
  cex::backtest::infra::ClickHouseConfig ch_cfg;
  ch_cfg.url = options.clickhouse_url;
  ch_cfg.database = options.clickhouse_db;
  ch_cfg.timeout_ms = 10000;
  cex::backtest::infra::ClickHouseReplayStorage clickhouse(ch_cfg, &storage_metrics);
  if (!clickhouse.EnsureSchema()) {
    std::cerr << "ClickHouse schema initialization failed\n";
    return 2;
  }

  const std::string run_id = "f15-" + std::to_string(NowMs());
  InMemorySessionRepo session_repo;
  cex::backtest::infra::InMemoryShadowLedger shadow_ledger(&storage_metrics);

  const std::vector<Scenario> scenarios = {
      {.name = "single_1000", .sessions = 1, .batches_per_session = options.single_batches},
      {.name = "single_10000", .sessions = 1, .batches_per_session = options.large_batches},
      {.name = "parallel_5x", .sessions = 5, .batches_per_session = options.parallel_batches},
      {.name = "parallel_20x", .sessions = 20, .batches_per_session = options.parallel_batches},
  };

  std::vector<ScenarioResult> results;
  results.reserve(scenarios.size());
  for (const auto& scenario : scenarios) {
    std::cout << "RUN " << scenario.name << " sessions=" << scenario.sessions
              << " batches_per_session=" << scenario.batches_per_session << std::endl;
    auto result = RunScenario(scenario, run_id, options, &session_repo, &clickhouse, &shadow_ledger);
    std::cout << "RESULT " << result.name
              << " wall_sec=" << Fixed(result.duration_sec)
              << " batches_per_sec=" << Fixed(result.batches_per_sec)
              << " ch_rows=" << result.clickhouse_rows
              << " ch_rows_per_sec=" << Fixed(result.clickhouse_rows_per_sec)
              << " kafka=" << result.kafka_consumed << "/" << result.expected_kafka_messages
              << " kafka_lag_messages=" << result.kafka_lag_messages
              << " solve_p95_ms=" << Fixed(result.solve_p95_ms)
              << " ch_p95_ms=" << Fixed(result.ch_insert_p95_ms)
              << " ledger_errors=" << result.ledger_errors
              << " completed=" << (result.all_completed ? "yes" : "no")
              << std::endl;
    results.push_back(result);
  }

  WriteReport(options, results);
  std::cout << "REPORT " << options.report_path << std::endl;
  return 0;
}
