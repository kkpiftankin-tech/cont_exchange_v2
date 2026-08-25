#include "app/replay_runtime_metrics.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <utility>

namespace cex::backtest::app {
namespace {

template <std::size_t N>
void ObserveHistogram(ReplayRuntimeMetrics::HistogramState<N>* hist,
                      const std::array<uint32_t, N>& buckets,
                      const double value) {
  if (hist == nullptr) return;
  ++hist->count;
  hist->sum += value;
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    if (value <= static_cast<double>(buckets[i])) {
      ++hist->bucket_counts[i];
    }
  }
}

std::string EscapeLabel(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      default: out.push_back(ch); break;
    }
  }
  return out;
}

template <std::size_t N>
void RenderHistogram(std::ostringstream& out,
                     const std::string& name,
                     const std::string& help,
                     const std::array<uint32_t, N>& buckets,
                     const ReplayRuntimeMetrics::HistogramState<N>& hist,
                     const std::string& labels = "") {
  out << "# HELP " << name << ' ' << help << "\n";
  out << "# TYPE " << name << " histogram\n";
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    out << name << "_bucket{";
    if (!labels.empty()) out << labels << ",";
    out << "le=\"" << buckets[i] << "\"} " << hist.bucket_counts[i] << "\n";
  }
  out << name << "_bucket{";
  if (!labels.empty()) out << labels << ",";
  out << "le=\"+Inf\"} " << hist.count << "\n";
  out << name << "_sum";
  if (!labels.empty()) out << '{' << labels << '}';
  out << ' ' << hist.sum << "\n";
  out << name << "_count";
  if (!labels.empty()) out << '{' << labels << '}';
  out << ' ' << hist.count << "\n";
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

void ReplayRuntimeMetrics::ObserveHistoryLoad(const double latency_ms,
                                              const std::size_t batches,
                                              const std::size_t fills,
                                              const std::size_t snapshots) {
  std::lock_guard<std::mutex> lock(mu_);
  ObserveHistogram(&history_load_latency_, kHistoryLoadBucketsMs, latency_ms);
  batches_loaded_total_ += static_cast<uint64_t>(batches);
  fills_loaded_total_ += static_cast<uint64_t>(fills);
  snapshots_loaded_total_ += static_cast<uint64_t>(snapshots);
}

void ReplayRuntimeMetrics::ObserveShadowNamespaceInit(const double latency_ms,
                                                      const bool success,
                                                      const bool reused) {
  std::lock_guard<std::mutex> lock(mu_);
  ObserveHistogram(&shadow_init_latency_, kShadowInitBucketsMs, latency_ms);
  if (!success) {
    ++shadow_namespace_init_total_["error"];
  } else if (reused) {
    ++shadow_namespace_init_total_["reused"];
  } else {
    ++shadow_namespace_init_total_["created"];
  }
}

void ReplayRuntimeMetrics::SetShadowNamespacesActive(const std::size_t active) {
  std::lock_guard<std::mutex> lock(mu_);
  shadow_namespaces_active_ = static_cast<double>(active);
}

void ReplayRuntimeMetrics::ObserveBatchExecution(const double batch_latency_ms,
                                                 const uint32_t solve_time_ms,
                                                 const int64_t progress_lag_ms,
                                                 const BatchOutcome outcome,
                                                 const FailureComponent failure_component,
                                                 const std::string& error_details) {
  std::lock_guard<std::mutex> lock(mu_);
  ObserveHistogram(&batch_latency_, kBatchLatencyBucketsMs, batch_latency_ms);
  ObserveHistogram(&solve_time_, kSolveTimeBucketsMs, static_cast<double>(solve_time_ms));
  ObserveHistogram(
      &progress_lag_, kProgressLagBucketsMs,
      static_cast<double>(std::max<int64_t>(0, progress_lag_ms)));

  if (outcome == BatchOutcome::kSoftFailure) {
    ++failures_total_["soft:" +
                      ClassifyFailureComponent(failure_component, error_details)];
  } else if (outcome == BatchOutcome::kHardFailure) {
    ++failures_total_["hard:" +
                      ClassifyFailureComponent(failure_component, error_details)];
  }
}

void ReplayRuntimeMetrics::ObserveFailure(const std::string& failure_type,
                                          const FailureComponent failure_component,
                                          const std::string& error_details) {
  std::lock_guard<std::mutex> lock(mu_);
  ++failures_total_[failure_type + ":" +
                    ClassifyFailureComponent(failure_component, error_details)];
}

void ReplayRuntimeMetrics::ObserveClickHouseInsert(const std::string& table,
                                                   const double latency_ms,
                                                   const bool success) {
  std::lock_guard<std::mutex> lock(mu_);
  ObserveHistogram(&clickhouse_insert_latency_[table], kClickHouseInsertBucketsMs, latency_ms);
  ++clickhouse_insert_total_[table + ":" + (success ? "ok" : "error")];
}

void ReplayRuntimeMetrics::ObserveKafkaPublish(const std::string& topic,
                                               const double latency_ms,
                                               const bool success) {
  std::lock_guard<std::mutex> lock(mu_);
  ObserveHistogram(&kafka_publish_latency_[topic], kKafkaPublishBucketsMs, latency_ms);
  ++kafka_publish_total_[topic + ":" + (success ? "ok" : "error")];
}

std::string ReplayRuntimeMetrics::RenderPrometheus() const {
  std::lock_guard<std::mutex> lock(mu_);

  std::ostringstream out;
  RenderHistogram(out,
                  "backtest_replay_history_load_latency_ms",
                  "Historical replay data load latency in milliseconds.",
                  kHistoryLoadBucketsMs,
                  history_load_latency_);
  RenderHistogram(out,
                  "backtest_replay_batch_latency_ms",
                  "Replay batch end-to-end execution latency in milliseconds.",
                  kBatchLatencyBucketsMs,
                  batch_latency_);
  RenderHistogram(out,
                  "backtest_replay_solve_time_ms",
                  "Replay solver time reported by batches in milliseconds.",
                  kSolveTimeBucketsMs,
                  solve_time_);
  RenderHistogram(out,
                  "backtest_replay_progress_lag_ms",
                  "Lag between replay publish time and source batch timestamp in milliseconds.",
                  kProgressLagBucketsMs,
                  progress_lag_);
  RenderHistogram(out,
                  "backtest_replay_shadow_namespace_init_latency_ms",
                  "Shadow namespace initialization latency in milliseconds.",
                  kShadowInitBucketsMs,
                  shadow_init_latency_);

  out << "# HELP backtest_replay_batches_loaded_total Historical batches loaded for replay.\n";
  out << "# TYPE backtest_replay_batches_loaded_total counter\n";
  out << "backtest_replay_batches_loaded_total " << batches_loaded_total_ << "\n";

  out << "# HELP backtest_replay_fills_loaded_total Historical fills loaded for replay.\n";
  out << "# TYPE backtest_replay_fills_loaded_total counter\n";
  out << "backtest_replay_fills_loaded_total " << fills_loaded_total_ << "\n";

  out << "# HELP backtest_replay_snapshots_loaded_total Historical snapshots loaded for replay.\n";
  out << "# TYPE backtest_replay_snapshots_loaded_total counter\n";
  out << "backtest_replay_snapshots_loaded_total " << snapshots_loaded_total_ << "\n";

  out << "# HELP backtest_replay_failures_total Replay failures by type and component.\n";
  out << "# TYPE backtest_replay_failures_total counter\n";
  for (const auto& [key, count] : failures_total_) {
    const auto split = key.find(':');
    const std::string failure_type = split == std::string::npos ? key : key.substr(0, split);
    const std::string component =
        split == std::string::npos ? "unknown" : key.substr(split + 1);
    out << "backtest_replay_failures_total{failure_type=\""
        << EscapeLabel(failure_type) << "\",component=\""
        << EscapeLabel(component) << "\"} " << count << "\n";
  }

  out << "# HELP backtest_replay_shadow_namespace_init_total Shadow namespace init outcomes.\n";
  out << "# TYPE backtest_replay_shadow_namespace_init_total counter\n";
  for (const auto& [result, count] : shadow_namespace_init_total_) {
    out << "backtest_replay_shadow_namespace_init_total{result=\""
        << EscapeLabel(result) << "\"} " << count << "\n";
  }

  out << "# HELP backtest_replay_shadow_namespaces_active Active shadow namespaces.\n";
  out << "# TYPE backtest_replay_shadow_namespaces_active gauge\n";
  out << "backtest_replay_shadow_namespaces_active " << shadow_namespaces_active_ << "\n";

  out << "# HELP backtest_replay_clickhouse_insert_total ClickHouse insert attempts by table and result.\n";
  out << "# TYPE backtest_replay_clickhouse_insert_total counter\n";
  for (const auto& [key, count] : clickhouse_insert_total_) {
    const auto split = key.rfind(':');
    const std::string table = split == std::string::npos ? key : key.substr(0, split);
    const std::string result = split == std::string::npos ? "unknown" : key.substr(split + 1);
    out << "backtest_replay_clickhouse_insert_total{table=\""
        << EscapeLabel(table) << "\",result=\""
        << EscapeLabel(result) << "\"} " << count << "\n";
  }
  for (const auto& [table, hist] : clickhouse_insert_latency_) {
    RenderHistogram(out,
                    "backtest_replay_clickhouse_insert_latency_ms",
                    "ClickHouse insert latency in milliseconds.",
                    kClickHouseInsertBucketsMs,
                    hist,
                    "table=\"" + EscapeLabel(table) + "\"");
  }

  out << "# HELP backtest_replay_kafka_publish_total Replay Kafka publish attempts by topic and result.\n";
  out << "# TYPE backtest_replay_kafka_publish_total counter\n";
  for (const auto& [key, count] : kafka_publish_total_) {
    const auto split = key.rfind(':');
    const std::string topic = split == std::string::npos ? key : key.substr(0, split);
    const std::string result = split == std::string::npos ? "unknown" : key.substr(split + 1);
    out << "backtest_replay_kafka_publish_total{topic=\""
        << EscapeLabel(topic) << "\",result=\""
        << EscapeLabel(result) << "\"} " << count << "\n";
  }
  for (const auto& [topic, hist] : kafka_publish_latency_) {
    RenderHistogram(out,
                    "backtest_replay_kafka_publish_latency_ms",
                    "Replay Kafka publish latency in milliseconds.",
                    kKafkaPublishBucketsMs,
                    hist,
                    "topic=\"" + EscapeLabel(topic) + "\"");
  }

  return out.str();
}

std::string ReplayRuntimeMetrics::ClassifyFailureComponent(
    const FailureComponent component,
    const std::string& error_details) {
  if (component != FailureComponent::kUnknown) {
    return FailureComponentName(component);
  }

  const std::string lower = Lower(error_details);
  if (Contains(lower, "solver")) return "solver";
  if (Contains(lower, "ledger")) return "ledger";
  if (Contains(lower, "shadow") || Contains(lower, "namespace")) return "shadow";
  if (Contains(lower, "clickhouse")) return "clickhouse";
  if (Contains(lower, "kafka")) return "kafka";
  if (Contains(lower, "marketdata")) return "marketdata";
  if (Contains(lower, "history")) return "history";
  if (Contains(lower, "config")) return "config";
  if (Contains(lower, "risk")) return "risk";
  return "pipeline";
}

}  // namespace cex::backtest::app
