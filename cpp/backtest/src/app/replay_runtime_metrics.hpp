#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "app/replay_orchestration_ports.hpp"

namespace cex::backtest::app {

class ReplayRuntimeMetrics {
 public:
  template <std::size_t N>
  struct HistogramState {
    std::array<uint64_t, N> bucket_counts{};
    uint64_t count{0};
    double sum{0.0};
  };

  void ObserveHistoryLoad(double latency_ms,
                          std::size_t batches,
                          std::size_t fills,
                          std::size_t snapshots);
  void ObserveShadowNamespaceInit(double latency_ms, bool success, bool reused);
  void SetShadowNamespacesActive(std::size_t active);
  void ObserveBatchExecution(double batch_latency_ms,
                             uint32_t solve_time_ms,
                             int64_t progress_lag_ms,
                             BatchOutcome outcome,
                             FailureComponent failure_component,
                             const std::string& error_details);
  void ObserveFailure(const std::string& failure_type,
                      FailureComponent failure_component,
                      const std::string& error_details);
  void ObserveClickHouseInsert(const std::string& table,
                               double latency_ms,
                               bool success);
  void ObserveKafkaPublish(const std::string& topic,
                           double latency_ms,
                           bool success);

  std::string RenderPrometheus() const;

 private:
  static constexpr std::array<uint32_t, 11> kHistoryLoadBucketsMs{
      10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 30000};
  static constexpr std::array<uint32_t, 11> kBatchLatencyBucketsMs{
      1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000};
  static constexpr std::array<uint32_t, 11> kSolveTimeBucketsMs{
      1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000};
  static constexpr std::array<uint32_t, 11> kProgressLagBucketsMs{
      10, 50, 100, 250, 500, 1000, 5000, 10000, 30000, 60000, 300000};
  static constexpr std::array<uint32_t, 10> kClickHouseInsertBucketsMs{
      5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000};
  static constexpr std::array<uint32_t, 9> kKafkaPublishBucketsMs{
      1, 5, 10, 25, 50, 100, 250, 500, 1000};
  static constexpr std::array<uint32_t, 9> kShadowInitBucketsMs{
      1, 5, 10, 25, 50, 100, 250, 500, 1000};

  static std::string ClassifyFailureComponent(FailureComponent component,
                                              const std::string& error_details);

  mutable std::mutex mu_;
  HistogramState<kHistoryLoadBucketsMs.size()> history_load_latency_{};
  HistogramState<kBatchLatencyBucketsMs.size()> batch_latency_{};
  HistogramState<kSolveTimeBucketsMs.size()> solve_time_{};
  HistogramState<kProgressLagBucketsMs.size()> progress_lag_{};
  HistogramState<kShadowInitBucketsMs.size()> shadow_init_latency_{};
  std::map<std::string, HistogramState<kClickHouseInsertBucketsMs.size()>>
      clickhouse_insert_latency_;
  std::map<std::string, HistogramState<kKafkaPublishBucketsMs.size()>>
      kafka_publish_latency_;
  std::map<std::string, uint64_t> failures_total_;
  std::map<std::string, uint64_t> clickhouse_insert_total_;
  std::map<std::string, uint64_t> kafka_publish_total_;
  std::map<std::string, uint64_t> shadow_namespace_init_total_;
  uint64_t batches_loaded_total_{0};
  uint64_t fills_loaded_total_{0};
  uint64_t snapshots_loaded_total_{0};
  double shadow_namespaces_active_{0.0};
};

}  // namespace cex::backtest::app
