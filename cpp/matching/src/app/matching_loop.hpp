#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "app/run_batch_uc.hpp"
#include "app/execution_intent_builder.hpp"
#include "app/external_venue_filter.hpp"
#include "app/planner_inputs_cache.hpp"
#include "app/solve_grouped_batch_use_case.hpp"
#include "app/solver_metrics.hpp"
#include "cex/common/kafka.hpp"
#include "fob/marketdata/v1/vector_liquidity.pb.h"  // F-05A (T-F05A-305 1a)
#include "domain/flow_order_repository.hpp"
#include "domain/grouped_solver_bisection.hpp"
#include "domain/solver_impl.hpp"
#include "infra/active_grouped_orders_loader.hpp"
#include "infra/execution_groups_producer.hpp"
#include "infra/postgres_execution_groups_repository.hpp"
#include "infra/postgres_child_graph_repository.hpp"
#include "infra/postgres_combo_compensation_repository.hpp"
#include "infra/market_data/market_data_client.hpp"
#include "fob/execution/v1/execution.pb.h"
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
               SolverMetrics& metrics,
               // F-09 (T-F09-048): при непустом DSN включается grouped-цикл
               // (combo multileg_vector_solver). Пусто → grouped выключен.
               const std::string& postgres_dsn = std::string());

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
  // F-05A (T-F05A-305 1a): consume marketdata.vectorized → solve → publish
  // matching.vector_clearing (диагностика; БЕЗ эмиссии денег/ledger).
  void on_vectorized_liquidity(const fob::marketdata::v1::VectorClearingInput& input);
  void on_venue_health(const fob::venue::v1::VenueHealth& health);
  // MVP-5 (ADR-037): провал внешней combo-ноги → combo_compensations(pending).
  void on_external_execution_report(const fob::execution::v1::ExecutionReport& report);
  domain::ExternalLiquidityBySymbol filtered_external_liquidity() const;
  domain::ExternalLiquidityBySymbol filtered_external_liquidity_unlocked() const;
  void run_one_batch();
  // F-09 (T-F09-048): grouped combo-цикл. Аддитивный, gated (grouped_enabled_),
  // обёрнут в try/catch — никогда не влияет на single-leg F-04 batch.
  void run_grouped_batch(const std::string& batch_id);
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

  // F-09 (T-F09-048): grouped combo execution (gated on postgres_dsn).
  // grouped_solver_ объявлен до solve_grouped_uc_ (тот держит на него ссылку).
  bool grouped_enabled_{false};
  domain::GroupedSolverBisection grouped_solver_;
  std::optional<SolveGroupedBatchUseCase> solve_grouped_uc_;
  std::optional<infra::ExecutionGroupsProducer> eg_producer_;  // обёртка над producer_
  std::unique_ptr<infra::PostgresActiveGroupsLoader> active_groups_loader_;
  std::unique_ptr<infra::PostgresExecutionGroupsRepository> eg_repo_;
  std::unique_ptr<infra::PostgresChildGraphRepository> child_graph_repo_;  // MVP-4 OCO/bracket
  std::unique_ptr<infra::PostgresComboCompensationRepository> compensation_repo_;  // MVP-5
  // F-09 rate-throttle внешних ног: leg_id с НЕподтверждённым external-intent.
  // Не шлём новый chunk, пока предыдущий не исполнен (отчёт venue) — иначе при
  // лаге venue возможен over-fill сверх q_max и двойной постинг в ledger (§17).
  std::set<std::string> external_in_flight_;
  std::mutex external_inflight_mu_;
};

}  // namespace cex::matching::app
