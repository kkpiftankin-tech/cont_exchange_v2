#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/execute_on_venue.hpp"
#include "app/liquidity_curve_producer.hpp"
#include "app/snapshot_producer.hpp"
#include "cex/common/kafka.hpp"
#include "domain/venue_adapter.hpp"
#include "infra/execution_report_producer.hpp"
#include "infra/kafka_message_publisher.hpp"
#include "infra/postgres_synthetic_order_repository.hpp"
#include "infra/venue_observability_producer.hpp"

namespace cex::venues::app {

struct VenueConfigRecord {
  std::string venue_id;
  std::string adapter_mode;
  std::string ws_url;
  std::string rest_base_url;
  std::string rpc_url;
  std::string chain_id;
  std::string pool_address;
  std::string venue_symbol;
  uint32_t depth_levels{20};
  std::string curve_level{"L3"};
  bool synthetic_enabled{false};
  uint32_t stale_threshold_ms{3000};
  bool circuit_breaker_enabled{true};
  uint32_t circuit_breaker_errors{10};
  uint32_t circuit_breaker_window_ms{30000};
  uint32_t circuit_breaker_cooldown_ms{30000};
  bool is_active{true};
  std::string routing_mode{"auto"};
  int64_t updated_at_ms{0};
};

struct VenueRuntimeMetrics {
  double stale_rate{0.0};
  double snapshots_per_sec{0.0};
  double last_curve_build_latency_ms{0.0};
  double last_curve_confidence{0.0};
  std::string last_curve_requested_level;
  std::string last_curve_effective_level;
  std::string last_curve_degradation_reason;
  std::string last_curve_quality_action;
};

// "Venues" service responsibilities in architecture:
// - adapters to external venues (CCXT in other language; here MVP simulator).
// - publish marketdata.raw
// - consume execution.intents and publish execution.venue (+ legacy execution.reports)
class VenuesLoop {
 public:
  explicit VenuesLoop(const std::string& brokers,
                      ISnapshotStorage* snapshot_storage = nullptr);

  void start();
  void stop();

  std::vector<VenueConfigRecord> ListVenueConfigs() const;
  std::optional<VenueConfigRecord> GetVenueConfig(const std::string& venue_id) const;
  bool UpsertVenueConfig(const VenueConfigRecord& config, std::string* error = nullptr);
  bool DeleteVenueConfig(const std::string& venue_id);
  bool ForceReconnect(const std::string& venue_id, std::string* error = nullptr);

  std::vector<domain::VenueHeartbeat> ListVenueHeartbeats() const;
  std::optional<domain::VenueHeartbeat> GetVenueHeartbeat(const std::string& venue_id) const;
  std::optional<VenueRuntimeMetrics> GetVenueRuntimeMetrics(const std::string& venue_id) const;
  std::optional<fob::venue::v1::VenueSnapshot> GetLastVenueSnapshot(
      const std::string& venue_id) const;
  std::optional<fob::venue::v1::VenueLiquidityCurve> GetLastVenueCurve(
      const std::string& venue_id) const;
  std::vector<fob::venue::v1::VenueSnapshot> GetVenueSnapshots(
      const std::string& venue_id, std::size_t limit) const;
  std::vector<fob::venue::v1::VenueLiquidityCurve> GetVenueCurves(
      const std::string& venue_id, std::size_t limit) const;
  std::vector<fob::orders::v1::SyntheticFlowOrder> GetVenueSynthetics(
      const std::string& venue_id, std::size_t limit) const;

 private:
  void md_publish_loop();
  void exec_consume_loop();
  void connect_and_subscribe_defaults();
  domain::VenueAdapter* find_adapter(const std::string& venue_id);
  void reload_producers_locked();
  void apply_runtime_config_locked(const VenueConfigRecord& config);

  std::string brokers_;
  ISnapshotStorage* snapshot_storage_{nullptr};
  cex::common::KafkaProducer producer_;
  cex::common::KafkaConsumer consumer_;
  infra::VenueObservabilityProducer observability_;
  std::unique_ptr<infra::ExecutionReportProducer> execution_report_producer_;
  std::unique_ptr<infra::KafkaMessagePublisher> kafka_publisher_;
  std::unique_ptr<infra::PostgresSyntheticOrderRepository> synthetic_order_repository_;
  std::vector<std::unique_ptr<domain::VenueAdapter>> adapters_;
  std::unique_ptr<SnapshotProducer> snapshot_producer_;
  std::unique_ptr<LiquidityCurveProducer> liquidity_curve_producer_;
  ExecuteOnVenue execute_on_venue_;
  SnapshotProducerConfig snapshot_config_{};
  LiquidityCurveProducerConfig curve_config_{};
  domain::VenueSubscription default_subscription_{};
  domain::VenueSnapshotRequest default_snapshot_request_{};
  mutable std::mutex runtime_mu_;
  std::unordered_map<std::string, VenueConfigRecord> venue_configs_;
  mutable std::mutex config_mu_;
  std::size_t history_capacity_{200};
  mutable std::mutex data_mu_;
  std::unordered_map<std::string, domain::VenueHeartbeat> last_heartbeats_;
  std::unordered_map<std::string, fob::venue::v1::VenueSnapshot> last_snapshots_;
  std::unordered_map<std::string, std::deque<fob::venue::v1::VenueSnapshot>> snapshot_history_;
  std::unordered_map<std::string, std::deque<fob::venue::v1::VenueLiquidityCurve>> curve_history_;
  std::unordered_map<std::string, std::deque<fob::orders::v1::SyntheticFlowOrder>> synthetic_history_;
  std::unordered_map<std::string, uint64_t> total_snapshots_by_venue_;
  std::unordered_map<std::string, uint64_t> stale_snapshots_by_venue_;
  std::unordered_map<std::string, VenueRuntimeMetrics> runtime_metrics_by_venue_;
  std::chrono::steady_clock::time_point md_started_at_{};
  std::size_t next_exec_adapter_idx_{0};

  std::atomic<bool> running_{false};
  std::thread t_md_;
  std::thread t_exec_;
};

}  // namespace cex::venues::app
