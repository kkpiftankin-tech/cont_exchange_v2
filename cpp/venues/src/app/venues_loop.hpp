#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/execute_on_venue.hpp"
#include "app/execution_report_sink.hpp"
#include "app/fill_diagnostics_sink.hpp"
#include "app/liquidity_curve_producer.hpp"
#include "app/sim_execution_assembler.hpp"
#include "app/sim_session_registry.hpp"
#include "app/venue_sim_router.hpp"
#include "app/depth_watch_registry.hpp"
#include "app/latest_wins_pool.hpp"
#include "app/poll_runner.hpp"
#include "app/depth_window_controller.hpp"
#include "app/snapshot_producer.hpp"
#include "cex/common/data_validity.hpp"
#include "cex/common/kafka.hpp"
#include "domain/venue_adapter.hpp"
#include "infra/execution_report_producer.hpp"
#include "infra/kafka_message_publisher.hpp"
#include "infra/postgres_child_order_repository.hpp"
#include "infra/postgres_hedgeflow_repository.hpp"
#include "infra/postgres_synthetic_order_repository.hpp"
#include "infra/venue_observability_producer.hpp"

namespace cex::venues::infra {
class CexWsRestAdapter;
}  // namespace cex::venues::infra

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

// Этап 0 (EXP-2026-10-15-exchange-requests): где уходит время цикла опроса одной пары, мс.
struct VenueStageTimings {
  double lock_wait_ms{0.0};     // ожидание per-venue мьютекса снимка
  double fetch_ms{0.0};         // RequestSnapshot целиком (REST + разбор)
  double rest_depth_ms{0.0};
  double rest_ticker_ms{0.0};
  double rest_trades_ms{0.0};
  double raw_publish_ms{0.0};   // marketdata.raw (стакан, мета, тикер)
  double snapshot_ms{0.0};      // нормализация + Kafka + запись в хранилище (ClickHouse)
  double curve_ms{0.0};         // расчёт и публикация кривой
  double total_ms{0.0};
  int rest_requests{0};
  int ticker_state{3};          // 0 запрошен, 1 пропущен (кэш), 2 нет токена, 3 выключен
  bool rest_valid{false};       // разбивка REST известна (снимок получен); иначе в DTO её нет
  int64_t recv_wall_ms{0};      // момент получения СВЕЖЕГО снимка; 0 ⇒ не обновлять прежнее значение
};

// ADR-078: состояние опроса пары и биржи под управлением планировщика (для админ-API и интерфейса).
struct VenuePollPairInfo {
  int health{3};            // 0 свежая, 1 стареющая, 2 устаревшая, 3 недоступна/нет данных
  double age_s{-1.0};
  int fails{0};
  double quarantine_s{0.0};
  double period_s{0.0};       // текущий период опроса пары (адаптивный, ADR-078 §7)
  double sigma_pm{-1.0};      // оценка σ пары, ‰/√с; -1 — оценки ещё нет
  double valid_age_s{-1.0};   // срок годности данных пары по допуску ε_soft, с; -1 — нет σ
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
  std::map<std::string, VenueStageTimings> stages;  // по символу ("BTC/USDT")
  double last_cycle_ms{0.0};     // полный проход poll_adapter по всем парам биржи
  double cycle_ms_ema{0.0};
  int cycle_pairs{0};
  int64_t cycle_end_wall_ms{0};
  // ADR-078: режим планировщика (заполняется при чтении метрик; poll_active=false — старый цикл)
  bool poll_active{false};
  double poll_period_s{0.0};
  double poll_scale{1.0};
  bool poll_paused{false};
  std::uint64_t poll_ok{0}, poll_retries{0}, poll_failures{0}, poll_missed{0};
  std::uint64_t poll_quarantines{0}, poll_venue_pauses{0}, poll_rate_limited{0};
  bool publish_async{false};                       // этап 2б: публикация через пул
  std::uint64_t publish_submitted{0}, publish_executed{0}, publish_coalesced{0};
  std::size_t publish_pending{0};
  std::map<std::string, VenuePollPairInfo> poll_pairs;   // по символу ("BTC/USDT")
};

// "Venues" service responsibilities in architecture:
// - adapters to external venues (CCXT in other language; here MVP simulator).
// - publish marketdata.raw
// - consume execution.intents and publish execution.venue (+ legacy execution.reports)
// F-18 #5/ADR-067 (T-F18-803): VenuesLoop реализует ExecutionReportSink, чтобы
// CexWsRestAdapter мог публиковать follow-up ExecutionReport резидентного
// (GTC) мейкер-лимита ТЕМ ЖЕ кодпутём (PublishExecutionReport), что и
// first-shot отчёт из exec_consume_loop — без расхождения Kafka/PG.
class VenuesLoop : public ExecutionReportSink {
 public:
  explicit VenuesLoop(const std::string& brokers,
                      ISnapshotStorage* snapshot_storage = nullptr);

  void start();
  void stop();
  // ADR-070: отдельное хранилище глубоких снимков быстрой полосы (короткий TTL).
  // Вызывать ДО start(). nullptr — быстрая полоса ничего не пишет.
  void SetLiveBookStorage(ISnapshotStorage* storage) { live_book_storage_ = storage; }

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
  // ADR-070 (дополнение): динамический режим. Панель присылает аренды (venue, symbol) —
  // быстрая полоса опрашивает только их и выключается сама, когда сигналы прекращаются.
  // venue="*" — все CEX-адаптеры. Неизвестные биржи/не-CEX и некорректные символы
  // отбрасываются (rejected_invalid). Лимит одновременных пар — DepthWatchRegistry.
  struct DepthWatchOutcome {
    DepthWatchRegistry::TouchResult touch;
    std::size_t rejected_invalid = 0;
  };
  DepthWatchOutcome WatchDepth(const std::vector<DepthWatchRegistry::Item>& items, int64_t ttl_ms);
  std::vector<DepthWatchRegistry::ActiveItem> ActiveDepthWatch() { return depth_watch_.Active(); }
  std::size_t DepthWatchMaxItems() const { return depth_watch_.max_items(); }

  // ADR-070: последний ГЛУБОКИЙ снимок (быстрая полоса) по (venue, symbol) — вход
  // расчёта кривых ликвидности «по запросу» (ComputeSlopeMethods). nullopt — нет.
  std::optional<fob::venue::v1::VenueSnapshot> GetDeepBook(
      const std::string& venue_id, const std::string& symbol) const;
  std::optional<fob::venue::v1::VenueLiquidityCurve> GetLastVenueCurve(
      const std::string& venue_id) const;
  std::vector<fob::venue::v1::VenueSnapshot> GetVenueSnapshots(
      const std::string& venue_id, std::size_t limit) const;
  std::vector<fob::venue::v1::VenueLiquidityCurve> GetVenueCurves(
      const std::string& venue_id, std::size_t limit) const;
  std::vector<fob::orders::v1::SyntheticFlowOrder> GetVenueSynthetics(
      const std::string& venue_id, std::size_t limit) const;

  // F-12 / IN-008 DoD-4 — externally owned PG repos for hedgeflows and
  // child_orders. Pointers may be null (degrades to "no-persistence" path
  // — VenuesLoop continues to publish to Kafka and PG simply doesn't
  // reflect HedgeFlow state).
  void SetHedgeflowRepository(infra::PostgresHedgeflowRepository* repo) {
    hedgeflow_repo_ = repo;
  }
  void SetChildOrderRepository(infra::PostgresChildOrderRepository* repo) {
    child_order_repo_ = repo;
  }
  // ADR-060 — навесить приёмник диагностики sim-fill на все CEX-адаптеры
  // (CexWsRestAdapter). Вызывается из main после конструктора (адаптеры уже
  // построены). DEX/AMM (uniswap) идёт через отдельный адаптер и здесь не
  // покрывается. nullptr допустим (диагностика отключена).
  void SetFillDiagnosticsSink(app::FillDiagnosticsSink* sink);

  // F-18 #5/ADR-067 (T-F18-803) — реализация ExecutionReportSink: публикует
  // follow-up отчёт резидентного (GTC) мейкер-лимита ТЕМ ЖЕ кодпутём
  // (PublishExecutionReport), что и first-shot отчёт из exec_consume_loop.
  void PublishFollowUpReport(
      const fob::execution::v1::ExecutionIntent& intent,
      const fob::execution::v1::ExecutionReport& report) override;

  // F-20 Phase 4 — live registry of active SimSessions, populated by the
  // sim.config consume loop (hot reload). Exposed so the VenueSimRouter
  // (next wiring step) can read routing decisions from it.
  SimSessionRegistry& sim_session_registry() { return sim_session_registry_; }

 private:
  void md_publish_loop();
  void exec_consume_loop();
  void sim_config_consume_loop();
  // F-09 UX: лёгкий тикер-фид по доп. символам (VENUES_FEED_SYMBOLS) — отдельно от
  // core md_publish_loop, чтобы не задеть F-11/F-12. Публикует только тикеры.
  void extra_ticker_loop();
  // ADR-070: быстрая полоса стакана (только REST depth → хранилище снимков) для панели
  // «Торговля». Выключена, если VENUES_FAST_DEPTH_VENUE пуст.
  void depth_fast_loop(const std::string& venue);
  // F-20 Phase 4 — SIM/SHADOW fork: simulate the child order against the
  // cached live LOB and publish the sim ExecutionReport + SimExecutionAnnotation
  // to the isolated sim.* topics (ADR-015). Applies the sampled venue latency
  // as a (capped) delay before publishing.
  void PublishSimExecution(const fob::execution::v1::ExecutionIntent& intent,
                           const RouteDecision& decision);
  // F-18 #5/ADR-067 (T-F18-803) — единый код-путь публикации ExecutionReport
  // (Normalize → liquidity_curve.ObserveExecution → execution_report_producer.Publish
  // → child_orders.ApplyReport → hedgeflows.ApplyReport → PublishTraffic → лог).
  // Вызывается из exec_consume_loop (first-shot) и PublishFollowUpReport (resting).
  void PublishExecutionReport(const fob::execution::v1::ExecutionIntent& intent,
                              const fob::execution::v1::ExecutionReport& rep);
  void connect_and_subscribe_defaults();
  domain::VenueAdapter* find_adapter(const std::string& venue_id);
  void reload_producers_locked();
  void apply_runtime_config_locked(const VenueConfigRecord& config);

  std::string brokers_;
  ISnapshotStorage* snapshot_storage_{nullptr};
  ISnapshotStorage* live_book_storage_{nullptr};
  DepthWatchRegistry depth_watch_;
  // ADR-077: окно глубины стакана по объёму для цикла клиринга (CE_DEPTH_WINDOW_ENABLED).
  std::unique_ptr<DepthWindowController> depth_window_;
  bool poll_scheduler_mode_{false};                          // ADR-078: VENUES_POLL_MODE=scheduler
  std::vector<std::unique_ptr<PollRunner>> poll_runners_;
  std::map<std::string, PollRunner*> poll_runner_by_venue_;  // venue_id -> исполнитель (только чтение после старта)
  std::unique_ptr<LatestWinsPool> publish_pool_;            // ADR-078 этап 2б: расчёт кривой отвязан от потоков опроса
  // ADR-078 §7: адаптивный период опроса пары по её волатильности (VENUES_POLL_ADAPTIVE=1).
  bool poll_adaptive_{false};
  double poll_eps_soft_{5e-4};                              // допуск ε_soft (доля цены): срок годности τ_ε = (ε/(zσ))²
  mutable std::mutex poll_vol_mu_;
  std::map<std::string, cex::common::DualHorizonVolatility> poll_vol_;  // "venue|symbol" -> σ по потоку mid этой биржи
  std::set<std::string> static_depth_venues_;  // биржи «всегда включено» (VENUES_FAST_DEPTH_VENUE)
  mutable std::mutex deep_books_mu_;
  std::unordered_map<std::string, fob::venue::v1::VenueSnapshot> deep_books_;  // "venue|symbol"
  cex::common::KafkaProducer producer_;
  cex::common::KafkaConsumer consumer_;
  infra::VenueObservabilityProducer observability_;
  std::unique_ptr<infra::ExecutionReportProducer> execution_report_producer_;
  std::unique_ptr<infra::KafkaMessagePublisher> kafka_publisher_;
  std::unique_ptr<infra::PostgresSyntheticOrderRepository> synthetic_order_repository_;
  // F-12 / IN-008 DoD-4 — non-owning pointers (lifetime managed by main.cpp).
  infra::PostgresHedgeflowRepository* hedgeflow_repo_{nullptr};
  infra::PostgresChildOrderRepository* child_order_repo_{nullptr};
  std::vector<std::unique_ptr<domain::VenueAdapter>> adapters_;
  std::unique_ptr<SnapshotProducer> snapshot_producer_;
  std::unique_ptr<LiquidityCurveProducer> liquidity_curve_producer_;
  ExecuteOnVenue execute_on_venue_;
  SimSessionRegistry sim_session_registry_;
  VenueSimRouter sim_router_{sim_session_registry_};
  SimExecutionAssembler sim_assembler_;
  SnapshotProducerConfig snapshot_config_{};
  LiquidityCurveProducerConfig curve_config_{};
  domain::VenueSubscription default_subscription_{};
  domain::VenueSnapshotRequest default_snapshot_request_{};
  // Инструменты sim (env VENUES_SIM_INSTRUMENTS): одна кривая на (venue, instrument).
  std::vector<fob::common::v1::Instrument> sim_instruments_{};
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

  // Runtime-порог устаревания venue-снапшота из PG (таблица f05a_clearing_config,
  // колонка venue_stale_ms). Настраивается из UI (вкладка Площадки). -1 = не задан
  // (используется env STALE_THRESHOLD_MS). TTL-поллинг в md_publish_loop.
  void RefreshVenueStaleFromPg();
 public:
  // Этап 0: запись таймингов пары и цикла биржи (вызывает poll_adapter).
  void RecordPairStage(const std::string& venue, const std::string& symbol,
                       VenueStageTimings timings);
  void RecordCycle(const std::string& venue, double cycle_ms, int pairs);
  // Контекст публикации снимка (общий для старого цикла и планировщика, ADR-078).
  struct SnapshotPublishContext {
    std::size_t legacy_depth_levels{20};
    std::size_t requested_levels{20};   // сколько уровней реально просили у площадки
    uint32_t stale_threshold_ms{15000};
    std::string requested_curve_level;
    bool synthetic_enabled{false};
  };
  // Публикует один свежий снимок: окно глубины, marketdata.raw, нормализация и хранилище, кривая, метрики.
  void ProcessFetchedSnapshot(domain::VenueAdapter* adapter, domain::VenueRawSnapshot& snapshot,
                              const SnapshotPublishContext& ctx, VenueStageTimings* st);

  // ADR-078 (этап 2а): опрос CEX-бирж под управлением планировщика (VENUES_POLL_MODE=scheduler).
  void StartPollRunners();
  void StopPollRunners();
  PollFetchResult PollFetchOnce(infra::CexWsRestAdapter* cex, std::size_t pair);
  // Свежая книга от исполнителя: ставит публикацию в пул «побеждает последнее» (этап 2б) или публикует сразу.
  void PollDeliver(infra::CexWsRestAdapter* cex, std::size_t pair, PollFetchResult&& res);
  std::optional<double> PollSigma(const std::string& venue, const std::string& symbol) const;
  void PollPublish(infra::CexWsRestAdapter* cex, std::size_t pair, PollFetchResult&& res);
  void PollTickDone(const std::string& venue, std::size_t pair, const PollFetchResult& last, bool ok);
  void PollHousekeeping(infra::CexWsRestAdapter* cex);
  bool VenueActive(const std::string& venue_id) const;
 private:
  std::string venue_stale_pg_dsn_{};
  std::atomic<int64_t> runtime_stale_ms_{-1};
  std::chrono::steady_clock::time_point last_stale_poll_{};

  // Параллельный REST-опрос: md_publish_loop опрашивает биржи конкурентно (по
  // адаптеру на задачу). poll_adapter — вынесенное тело одного адаптера.
  // snapshot_mutex_for — per-venue мьютекс вокруг RequestSnapshot: разные биржи
  // не конфликтуют, а общий binance-адаптер (md_publish_loop vs extra_ticker_loop)
  // сериализуется на одном мьютексе (rest_client_ handle не потокобезопасен).
  void poll_adapter(domain::VenueAdapter* adapter);
  std::mutex& snapshot_mutex_for(const std::string& venue_id);
  std::unordered_map<std::string, std::unique_ptr<std::mutex>> snapshot_mu_by_venue_;

  std::atomic<bool> running_{false};
  std::thread t_md_;
  std::thread t_exec_;
  std::thread t_sim_config_;
  std::thread t_extra_ticker_;
  std::vector<std::thread> t_depth_fast_;  // по потоку на биржу (VENUES_FAST_DEPTH_VENUE)
  // Сериализует RequestSnapshot между md_publish_loop и extra_ticker_loop
  // (общий binance-адаптер; rest_client_->Get не потокобезопасен на shared handle).
  std::mutex snapshot_mu_;
};

}  // namespace cex::venues::app
