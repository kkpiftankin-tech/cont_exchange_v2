#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "app/run_batch_uc.hpp"
#include "app/execution_intent_builder.hpp"
#include "app/external_venue_filter.hpp"
#include "app/planner_inputs_cache.hpp"
#include "app/solver_metrics.hpp"
#include "cex/common/kafka.hpp"
#include "domain/flow_order_repository.hpp"
#include "domain/solver_impl.hpp"
#include "infra/market_data/market_data_client.hpp"
#include "fob/matching/v1/batch.pb.h"
#include "infra/market_data/market_data_client.hpp"
#include "fob/orders/v1/orders.pb.h"
#include "fob/venue/v1/venue.pb.h"

#include "domain/solver_config_ports.hpp"

namespace cex::matching::app {

// Periodic matching loop:
// - consumes orders.normalized for in-memory fallback mode
// - reads solver_config before each batch
// - loads active flow orders from repository when Postgres is enabled
// - produces batch.outputs (BatchResult)
class MatchingLoop {
 public:
  MatchingLoop(const std::string& brokers,
               int batch_interval_ms,
               std::shared_ptr<domain::IFlowOrderRepository> flow_order_repository,
               std::unique_ptr<domain::SolverConfigRepositoryPort> solver_config_repo,
               std::shared_ptr<infra::MarketDataClient> market_data_client,
               SolverMetrics& metrics);

  void start();
  void stop();

  struct OrderSnapshot {
    std::string order_id;
    std::string status;        // pending|partial|filled|cancelled|expired|unknown
    double total_qty{0.0};
    double filled_qty{0.0};
  };
  std::optional<OrderSnapshot> snapshot_order(const std::string& order_id) const;

 private:
  void consume_orders_loop();
  void batch_timer_loop();
  int refresh_batch_interval_ms();

  void on_order_event(const fob::orders::v1::OrdersNormalized& evt);
  void on_liquidity_curve(const fob::venue::v1::VenueLiquidityCurve& curve);
  void on_venue_health(const fob::venue::v1::VenueHealth& health);
  domain::ExternalLiquidityBySymbol filtered_external_liquidity() const;
  domain::ExternalLiquidityBySymbol filtered_external_liquidity_unlocked() const;
  void run_one_batch();
  bool publish_batch(const fob::matching::v1::BatchResult& batch);

  std::string brokers_;
  int batch_interval_ms_;

  cex::common::KafkaProducer producer_;
  cex::common::KafkaConsumer consumer_;
  domain::ContinuousClearingSolver solver_;
  SolverMetrics& metrics_;
  RunBatchUseCase run_batch_uc_;

  std::unique_ptr<domain::SolverConfigRepositoryPort> solver_config_repo_;
  std::shared_ptr<infra::MarketDataClient> market_data_client_;

  std::atomic<bool> running_{false};
  std::thread t_consume_;
  std::thread t_batch_;
  std::shared_ptr<domain::IFlowOrderRepository> flow_order_repository_;

  // Active orders by order_id
  std::unordered_map<std::string, domain::FlowOrder> active_;

  mutable std::mutex order_index_mutex_;
  std::unordered_map<std::string, OrderSnapshot> order_index_;
  void index_order(const domain::FlowOrder& order, const std::string& status);
  void index_terminal(const std::string& order_id, const std::string& status,
                      double filled_qty);

  mutable std::mutex planner_inputs_cache_mutex_;
  PlannerInputsCache planner_inputs_cache_;
  ExecutionIntentBuilder execution_intent_builder_;
};

}  // namespace cex::matching::app
