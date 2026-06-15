// ============================================================================
// matching_loop.cpp — orchestration ядро F-04: запускает batch cycle и
// поддерживает Kafka consumer для входящих заявок / venue.liquidity / venue.health.
//
// Назначение и физический смысл:
//   MatchingLoop — главный класс matching-сервиса. Управляет:
//     * двумя background threads: consume_orders_loop (Kafka consumer) и
//       batch_timer_loop (periodic ticker, по умолчанию 5 сек);
//     * глобальным state: active_ (in-memory FlowOrder map), order_index_
//       (UI-friendly snapshot для /orders/<id> endpoint), planner_inputs_cache_
//       (external venue liquidity + health);
//     * вызывает RunBatchUseCase → solver → publish → ledger;
//     * F-12: hedge trigger evaluation + multi-venue routing (DoD-2 PR-F12-15) +
//       execution intent publication.
//
// Архитектура thread'ов:
//   * consume_orders_loop: poll Kafka 500ms, маршрутизирует topic → handler.
//     Topics: "orders.normalized", "venue.liquidity.fob", "venue.health".
//   * batch_timer_loop: sleep(batch_interval_ms_) → run_one_batch().
//     Interval может hot-reload'иться через solver_config_repo_.
//
// Источник flow_orders:
//   * Если flow_order_repository_ задан (MATCHING_POSTGRES_DSN) → PG (canonical).
//   * Иначе → in-memory active_ (Kafka-fed legacy path).
//
// Ключевые исторические фиксы:
//   * PR-F02-003: order_index_ status sync с расширением switch на все
//     7 FlowOrderStatus значений + guard на filled_qty >= total_qty.
//   * PR-F02-006: order_updates pre-pass читает batch.order_updates() ДО
//     refresh active_orders — иначе race теряет terminal updates.
//   * PR-F12-5: HedgeTriggerConfig / HedgeExecutionIntentConfig из env.
//   * PR-F12-15 (F-12 DoD-2): multi-venue routing — split target_qty
//     proportionally to L(v) между allowed_venues.
//
// CRITICAL: ВСЁ что mutate state требует locking. order_index_mutex_ — для
// order_index_; planner_inputs_cache_mutex_ — для planner_inputs_cache_.
// active_ — НЕ под mutex'ом (single-thread access из batch loop + consumer
// гарантирует исключаемость через design).
// ============================================================================

#include "app/matching_loop.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fob/execution/v1/execution.pb.h"

#include "app/execution_planner.hpp"
#include "app/combo_external_routing.hpp"
#include "app/hedge_execution_intents_publisher.hpp"
#include "cex/common/decimal.hpp"
#include "cex/common/env.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "cex/common/uuid.hpp"
#include "fob/venue/v1/venue.pb.h"
#include "infra/kafka/batch_outputs_producer.hpp"
#include "infra/kafka/execution_intents_producer.hpp"

namespace cex::matching::app {

namespace {

/// Преобразование vector<FlowOrder> → map<order_id, FlowOrder> для быстрого
/// lookup'а. Reserve для O(1) bucket allocation.
std::unordered_map<std::string, domain::FlowOrder> MapRepoOrders(
    const std::vector<domain::FlowOrder>& repo_orders) {
  std::unordered_map<std::string, domain::FlowOrder> result;
  result.reserve(repo_orders.size());
  for (const auto& order : repo_orders) {
    result[order.order_id] = order;
  }
  return result;
}

/// Snapshot map → vector — для передачи в solver / market_data_client.
/// Copy-by-value (FlowOrder copyable) — изменения в snapshot'е не влияют
/// на исходную map.
std::vector<domain::FlowOrder> SnapshotActiveOrders(
    const std::unordered_map<std::string, domain::FlowOrder>& active_orders) {
  std::vector<domain::FlowOrder> snapshot;
  snapshot.reserve(active_orders.size());
  for (const auto& [order_id, order] : active_orders) {
    (void)order_id;
    snapshot.push_back(order);
  }
  return snapshot;
}

/// Normalized price limits для planner-forecast. nullopt = side disabled.
struct NormalizedOrderPriceLimits {
  std::optional<double> buy_limit_price;
  std::optional<double> sell_limit_price;
};

/// Извлекает [normalized_low, normalized_high] с учётом legacy signed-by-side
/// convention (PR-F02-005): для SELL заявок p_low/p_high stored as negated,
/// здесь возвращаем business-facing positive values.
NormalizedOrderPriceLimits NormalizeOrderPriceLimits(
    const domain::FlowOrder& order) {
  const double raw_low = static_cast<double>(order.p_low);
  const double raw_high = static_cast<double>(order.p_high);
  if (!std::isfinite(raw_low) || !std::isfinite(raw_high)) {
    return {};
  }

  double normalized_low = raw_low;
  double normalized_high = raw_high;

  // Legacy SELL orders are stored as inverted-sign interval:
  // p_low = -price_high, p_high = -price_low.
  if (raw_low < 0.0 && raw_high < 0.0) {
    normalized_low = -raw_high;
    normalized_high = -raw_low;
  }

  if (normalized_low > normalized_high) {
    std::swap(normalized_low, normalized_high);
  }

  NormalizedOrderPriceLimits limits;
  if (normalized_high > 0.0) {
    limits.buy_limit_price = normalized_high;
  }
  if (normalized_low > 0.0) {
    limits.sell_limit_price = normalized_low;
  }

  return limits;
}

// F-12 / PR-F12-5: parse a numeric string like "0.05" or "50000" into a
// fixed-point Decimal{units, scale}. Empty / unparseable / non-finite input
// returns zero (which the policy treats as "threshold disabled").
//
/// Дубликат ParseDecimalString из cpp/risk/src/app/risk_uc.cpp — намеренно
/// не вынесен в common чтобы избежать cross-service refactor на PR-F12-5.
/// При будущем cleanup'е стоит вынести в cex::common.
cex::common::Decimal ParseDecimalString(const std::string& raw) {
  if (raw.empty()) return cex::common::Decimal::zero();
  std::string s = raw;
  bool negative = false;
  if (s[0] == '-') {
    negative = true;
    s.erase(0, 1);
  } else if (s[0] == '+') {
    s.erase(0, 1);
  }
  if (s.empty()) return cex::common::Decimal::zero();

  const auto dot = s.find('.');
  std::string int_part;
  std::string frac_part;
  if (dot == std::string::npos) {
    int_part = s;
  } else {
    int_part = s.substr(0, dot);
    frac_part = s.substr(dot + 1);
  }
  // strip trailing zeros from fractional part to keep scale minimal
  while (!frac_part.empty() && frac_part.back() == '0') frac_part.pop_back();
  if (int_part.empty()) int_part = "0";

  const std::string combined = int_part + frac_part;
  try {
    int64_t units = std::stoll(combined);
    if (negative) units = -units;
    return cex::common::Decimal{units, static_cast<int32_t>(frac_part.size())};
  } catch (...) {
    return cex::common::Decimal::zero();
  }
}

// F-12 / PR-F12-5: convert "BTC/USDT" → "BTC_USDT" for env-var suffix.
//
/// Env vars не могут содержать '/' или '-'. Конвертируем для построения
/// per-symbol env var names типа HEDGE_TRIGGER_QTY_BTC_USDT.
std::string SymbolToEnvSuffix(const std::string& symbol) {
  std::string out = symbol;
  for (auto& c : out) {
    if (c == '/' || c == '-') c = '_';
  }
  return out;
}

// F-12 / PR-F12-5: load HedgeTriggerConfig from env. Defaults are zero, which
// disables the trigger entirely (matches pre-PR behaviour). Active deployments
// set HEDGE_TRIGGER_QTY_DEFAULT / HEDGE_TRIGGER_NOTIONAL_DEFAULT, or per-symbol
// HEDGE_TRIGGER_QTY_BTC_USDT etc., and list active symbols in HEDGE_TRIGGER_SYMBOLS.
//
/// Per-symbol thresholds OVERRIDE default. Если HEDGE_TRIGGER_SYMBOLS пуст —
/// per_symbol_thresholds пустой, и применяется только default (если он > 0).
HedgeTriggerConfig LoadHedgeTriggerConfig() {
  HedgeTriggerConfig config;
  config.default_thresholds.threshold_qty = ParseDecimalString(
      cex::common::Env::get_string("HEDGE_TRIGGER_QTY_DEFAULT", "0"));
  config.default_thresholds.threshold_notional = ParseDecimalString(
      cex::common::Env::get_string("HEDGE_TRIGGER_NOTIONAL_DEFAULT", "0"));

  const auto symbols_csv =
      cex::common::Env::get_string("HEDGE_TRIGGER_SYMBOLS", "");
  if (symbols_csv.empty()) return config;

  size_t start = 0;
  while (start <= symbols_csv.size()) {
    auto end = symbols_csv.find(',', start);
    if (end == std::string::npos) end = symbols_csv.size();
    std::string symbol = symbols_csv.substr(start, end - start);
    // trim whitespace
    while (!symbol.empty() && std::isspace(static_cast<unsigned char>(symbol.front()))) symbol.erase(symbol.begin());
    while (!symbol.empty() && std::isspace(static_cast<unsigned char>(symbol.back()))) symbol.pop_back();
    if (!symbol.empty()) {
      const auto suffix = SymbolToEnvSuffix(symbol);
      HedgeTriggerThreshold threshold;
      threshold.threshold_qty = ParseDecimalString(
          cex::common::Env::get_string("HEDGE_TRIGGER_QTY_" + suffix, "0"));
      threshold.threshold_notional = ParseDecimalString(
          cex::common::Env::get_string("HEDGE_TRIGGER_NOTIONAL_" + suffix, "0"));
      config.per_symbol_thresholds.emplace(symbol, threshold);
    }
    start = end + 1;
  }
  return config;
}

// F-12 / PR-F12-5: parse comma-separated string into a vector, trimming
// surrounding whitespace per item and skipping empties.
std::vector<std::string> ParseCsvList(const std::string& raw) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= raw.size()) {
    auto end = raw.find(',', start);
    if (end == std::string::npos) end = raw.size();
    std::string item = raw.substr(start, end - start);
    while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
    while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
    if (!item.empty()) out.push_back(std::move(item));
    start = end + 1;
  }
  return out;
}

fob::execution::v1::ExecutionUrgency ParseUrgency(const std::string& raw) {
  if (raw == "LOW") return fob::execution::v1::URGENCY_LOW;
  if (raw == "HIGH") return fob::execution::v1::URGENCY_HIGH;
  return fob::execution::v1::URGENCY_MEDIUM;
}

fob::execution::v1::ExecutionStrategy ParseStrategy(const std::string& raw) {
  if (raw == "LIMIT") return fob::execution::v1::EXEC_STRATEGY_LIMIT;
  if (raw == "TWAP") return fob::execution::v1::EXEC_STRATEGY_TWAP;
  if (raw == "POST_ONLY") return fob::execution::v1::EXEC_STRATEGY_POST_ONLY;
  return fob::execution::v1::EXEC_STRATEGY_MARKET;
}

fob::common::v1::TimeInForce ParseTif(const std::string& raw) {
  if (raw == "GTC") return fob::common::v1::TIF_GTC;
  if (raw == "FOK") return fob::common::v1::TIF_FOK;
  return fob::common::v1::TIF_IOC;
}

// F-12 / PR-F12-5: load HedgeExecutionIntentConfig from env.
HedgeExecutionIntentConfig LoadHedgeExecutionIntentConfig() {
  HedgeExecutionIntentConfig config;
  config.urgency = ParseUrgency(
      cex::common::Env::get_string("HEDGE_INTENT_URGENCY", "MEDIUM"));
  config.strategy = ParseStrategy(
      cex::common::Env::get_string("HEDGE_INTENT_STRATEGY", "MARKET"));
  config.tif = ParseTif(cex::common::Env::get_string("HEDGE_INTENT_TIF", "IOC"));
  config.timeout_ms = static_cast<int64_t>(
      cex::common::Env::get_int("HEDGE_INTENT_TIMEOUT_MS", 30000));
  config.max_slippage_bps = static_cast<int32_t>(
      cex::common::Env::get_int("HEDGE_INTENT_MAX_SLIPPAGE_BPS", 50));
  config.allowed_venues = ParseCsvList(
      cex::common::Env::get_string("HEDGE_INTENT_ALLOWED_VENUES", ""));
  return config;
}

VenueThresholds LoadVenueHealthThresholds() {
  VenueThresholds thresholds;
  auto get_double = [](const char* name, double def) -> double {
    const auto raw = cex::common::Env::try_get_string(name);
    if (!raw.has_value()) {
      return def;
    }
    try {
      double value = std::stod(*raw);
      return std::clamp(value, 0.0, 1.0);
    } catch (...) {
      return def;
    }
  };

  thresholds.min_health_score =
      get_double("MATCHING_MIN_VENUE_HEALTH_SCORE", 0.5);
  thresholds.min_confidence = get_double("MATCHING_MIN_VENUE_CONFIDENCE", 0.2);
  thresholds.confidence_for_l3 = get_double("MATCHING_CONFIDENCE_FOR_L3", 0.8);
  thresholds.confidence_for_l2 = get_double("MATCHING_CONFIDENCE_FOR_L2", 0.5);

  return thresholds;
}

double MaxCurveQty(const fob::venue::v1::SideLiquidityCurve& curve) {
  if (curve.q_grid_size() <= 0) {
    return 0.0;
  }
  return curve.q_grid(curve.q_grid_size() - 1);
}

std::string CurveIdForLog(const fob::venue::v1::VenueLiquidityCurve& curve) {
  if (!curve.curve_id().empty()) {
    return curve.curve_id();
  }
  if (curve.has_meta() && !curve.meta().event_id().empty()) {
    return curve.meta().event_id();
  }
  return {};
}
}  // namespace

// ============================================================================
// MatchingLoop конструктор.
//
// Принимает зависимости через DI:
//   brokers              — Kafka broker list "host1:9092,host2:9092".
//   batch_interval_ms    — interval между batch tick'ами (5000 default).
//   flow_order_repository — nullable, для PG-режима чтения flow_orders.
//   solver_config_repo   — для hot-reload solver config из PG (nullable).
//   market_data_client   — для reference prices fetch (nullable).
//   metrics              — Prometheus metrics обёртка.
//
// run_batch_uc_ инициализируется с lambda как publish_batch callback —
// она и пишет в Kafka, и записывает metrics.
// ============================================================================
MatchingLoop::MatchingLoop(
    const std::string& brokers,
    int batch_interval_ms,
    std::shared_ptr<domain::IFlowOrderRepository> flow_order_repository,
    std::unique_ptr<domain::SolverConfigRepositoryPort> solver_config_repo,
    std::shared_ptr<infra::MarketDataClient> market_data_client,
    SolverMetrics& metrics,
    const std::string& postgres_dsn)
    : brokers_(brokers),
      batch_interval_ms_(batch_interval_ms),
      producer_({.brokers = brokers, .client_id = "matching"}),
      consumer_({.brokers = brokers,
                 .group_id = "matching",
                 .client_id = "matching",
                 .enable_auto_commit = false}),
      metrics_(metrics),
      run_batch_uc_(solver_,
                    [this](const fob::matching::v1::BatchResult& batch) {
                      metrics_.ObserveBatch(batch);
                      return publish_batch(batch);
                    },
                    LoadHedgeTriggerConfig(),
                    LoadHedgeExecutionIntentConfig()),
      solver_config_repo_(std::move(solver_config_repo)),
      market_data_client_(std::move(market_data_client)),
      flow_order_repository_(std::move(flow_order_repository)),
      planner_inputs_cache_(LoadVenueHealthThresholds()) {
  // F-09 (T-F09-048): включаем grouped combo-цикл при заданном PG DSN.
  // Любая ошибка инициализации → grouped выключен, single-leg F-04 не затронут.
  if (!postgres_dsn.empty()) {
    try {
      active_groups_loader_ =
          std::make_unique<infra::PostgresActiveGroupsLoader>(postgres_dsn);
      eg_repo_ = std::make_unique<infra::PostgresExecutionGroupsRepository>(postgres_dsn);
      child_graph_repo_ = std::make_unique<infra::PostgresChildGraphRepository>(postgres_dsn);
      compensation_repo_ = std::make_unique<infra::PostgresComboCompensationRepository>(postgres_dsn);
      eg_producer_.emplace(producer_);
      solve_grouped_uc_.emplace(grouped_solver_);
      grouped_enabled_ = true;
      cex::common::log_json("INFO", "Matching F-09 grouped execution enabled", {});
    } catch (const std::exception& ex) {
      grouped_enabled_ = false;
      cex::common::log_json("ERROR",
                            "Failed to enable F-09 grouped execution; staying single-leg only",
                            {{"error", ex.what()}});
    }
  }
}

// ============================================================================
// start — запуск consumer + batch loop threads.
//
// Шаги:
//   1. Log effective F-12 config (для operator confirmation).
//   2. running_ → true.
//   3. consumer_.subscribe три topic.
//   4. Spawn 2 threads: consume_orders_loop, batch_timer_loop.
//
// CRITICAL: вызывать только один раз. Повторный start без stop() leak'нет threads.
// ============================================================================
void MatchingLoop::start() {
  // F-12 / PR-F12-5: log effective hedge trigger config + intent config so
  // operators can confirm thresholds without restarting the service.
  const auto trigger_cfg = LoadHedgeTriggerConfig();
  const auto intent_cfg = LoadHedgeExecutionIntentConfig();
  std::string venues_csv;
  for (size_t i = 0; i < intent_cfg.allowed_venues.size(); ++i) {
    if (i > 0) venues_csv += ",";
    venues_csv += intent_cfg.allowed_venues[i];
  }
  std::map<std::string, std::string> log_fields = {
      {"default_threshold_qty",
       trigger_cfg.default_thresholds.threshold_qty.to_string()},
      {"default_threshold_notional",
       trigger_cfg.default_thresholds.threshold_notional.to_string()},
      {"intent_urgency", std::to_string(static_cast<int>(intent_cfg.urgency))},
      {"intent_strategy", std::to_string(static_cast<int>(intent_cfg.strategy))},
      {"intent_tif", std::to_string(static_cast<int>(intent_cfg.tif))},
      {"intent_timeout_ms", std::to_string(intent_cfg.timeout_ms)},
      {"intent_max_slippage_bps", std::to_string(intent_cfg.max_slippage_bps)},
      {"intent_allowed_venues", venues_csv},
      {"per_symbol_overrides_count",
       std::to_string(trigger_cfg.per_symbol_thresholds.size())}};
  for (const auto& [sym, th] : trigger_cfg.per_symbol_thresholds) {
    log_fields["sym:" + sym + ":qty"] = th.threshold_qty.to_string();
    log_fields["sym:" + sym + ":notional"] = th.threshold_notional.to_string();
  }
  cex::common::log_json("INFO", "F-12 hedge config loaded", log_fields);

  running_.store(true);
  consumer_.subscribe(
      {"orders.normalized", "venue.liquidity.fob", "venue.health", "execution.venue"});
  t_consume_ = std::thread([this] { consume_orders_loop(); });
  t_batch_ = std::thread([this] { batch_timer_loop(); });
}

/// Graceful shutdown: clear running_ flag, ждём threads.
/// join() — блокирующий, но threads должны завершиться в течение 500ms
/// (один Kafka poll cycle) или batch_interval_ms_ (sleep period).
void MatchingLoop::stop() {
  running_.store(false);
  if (t_consume_.joinable()) {
    t_consume_.join();
  }
  if (t_batch_.joinable()) {
    t_batch_.join();
  }
}

// ============================================================================
// consume_orders_loop — Kafka consumer thread.
//
// Polls 500ms за раз. Маршрутизирует payload в handler по topic:
//   "orders.normalized" → on_order_event (create/cancel/amend).
//   "venue.liquidity.fob" → on_liquidity_curve (F-11 LOB→FOB curves).
//   "venue.health" → on_venue_health (F-11 venue circuit-breaker state).
//
// poll_once возвращает false при transport-level fail → break.
// ============================================================================
void MatchingLoop::consume_orders_loop() {
  while (running_.load()) {
    bool ok = consumer_.poll_once(
        500,
        [this](const std::string& topic,
               const std::string& key,
               const std::string& payload) {
          (void)key;
          if (topic == "orders.normalized") {
            fob::orders::v1::OrdersNormalized evt;
            if (!cex::common::from_bytes(payload, evt)) {
              cex::common::log_json("ERROR",
                                    "Failed to parse OrdersNormalized");
              return;
            }
            on_order_event(evt);
            return;
          }

          if (topic == "venue.liquidity.fob") {
            fob::venue::v1::VenueLiquidityCurve curve;
            if (!cex::common::from_bytes(payload, curve)) {
              cex::common::log_json("ERROR",
                                    "Failed to parse VenueLiquidityCurve");
              return;
            }
            on_liquidity_curve(curve);
            return;
          }

          if (topic == "venue.health") {
            const auto health = ParseVenueHealthMessage(payload);
            if (!health.has_value()) {
              cex::common::log_json("ERROR",
                                    "Failed to parse venue.health payload");
              return;
            }
            on_venue_health(*health);
            return;
          }

          // MVP-5 (ADR-037): провал внешней combo-ноги → требование компенсации.
          if (topic == "execution.venue") {
            fob::execution::v1::ExecutionReport report;
            if (!cex::common::from_bytes(payload, report)) {
              cex::common::log_json("ERROR", "Failed to parse ExecutionReport (execution.venue)");
              return;
            }
            on_external_execution_report(report);
            return;
          }
        });
    if (!ok) {
      break;
    }
  }
}

// on_external_execution_report — MVP-5 (ADR-037). Провал внешней combo-ноги
// (rejected/cancelled/expired) фиксируется как combo_compensations(pending).
// Успех/partial игнорируются (ledger постит). Не-combo internal_order_id (hedge)
// отсеиваются по FindComboLegParent. Идемпотентно по report_id.
void MatchingLoop::on_external_execution_report(
    const fob::execution::v1::ExecutionReport& report) {
  if (!compensation_repo_) {
    return;
  }
  // report не несёт internal_order_id; линковка через client_order_id (= leg_id,
  // выставлен в BuildExternalIntent, эхо venues).
  const std::string& leg_id = report.client_order_id();
  if (leg_id.empty()) {
    return;
  }

  const auto st = report.status();
  const bool is_fill = st == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
  const char* reason = nullptr;
  switch (st) {
    case fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED: reason = "rejected"; break;
    case fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED: reason = "cancelled"; break;
    case fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED: reason = "expired"; break;
    case fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED: break;  // терминальный успех
    default: return;  // NEW / PARTIALLY_FILLED → не терминально
  }
  try {
    const auto parent = compensation_repo_->FindComboLegParent(leg_id);
    if (!parent.has_value()) {
      return;  // не combo-нога (hedge / прочий internal_order_id)
    }

    if (is_fill) {
      // MVP-5 fix: внешняя нога исполнена → 'filled' (loader перестаёт re-routить).
      // Идемпотентно: только первый active→filled применяет filled_cum.
      if (compensation_repo_->MarkExternalLegFilled(
              leg_id, cex::common::Decimal::from_proto(report.filled_qty()))) {
        cex::common::log_json("INFO", "Combo external leg filled",
                              {{"parent_order_id", *parent}, {"leg_id", leg_id}});
      }
      return;
    }

    // reject/cancel/expire: компенсацию пишем ТОЛЬКО на первый терминальный переход
    // ноги (active→failed_external) → ровно ОДНА компенсация на ногу, без роста от
    // повторного routing'а (MVP-5 fix). Loader исключает failed_external.
    if (compensation_repo_->MarkExternalLegFailed(leg_id)) {
      infra::ComboCompensation c;
      c.parent_order_id = *parent;
      c.leg_id = leg_id;
      c.report_id = report.report_id();
      c.reason = reason;
      c.internal_filled_qty = cex::common::Decimal::from_proto(report.filled_qty());
      compensation_repo_->RecordPending(c);
      cex::common::log_json("WARN", "Combo external leg failed — compensation pending",
                            {{"parent_order_id", *parent},
                             {"leg_id", leg_id},
                             {"reason", reason},
                             {"report_id", c.report_id}});
    }
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Compensation check failed",
                          {{"leg_id", leg_id}, {"error", ex.what()}});
  }
}

// ============================================================================
// batch_timer_loop — periodic batch ticker thread.
//
// sleep(batch_interval_ms_) → run_one_batch() → repeat.
// batch_interval_ms_ может hot-reload'иться внутри refresh_batch_interval_ms()
// из solver_config_repo_ — это позволяет operator изменять interval без
// рестарта сервиса.
// ============================================================================
void MatchingLoop::batch_timer_loop() {
  using namespace std::chrono;
  while (running_.load()) {
    std::this_thread::sleep_for(milliseconds(refresh_batch_interval_ms()));
    if (!running_.load()) {
      break;
    }
    run_one_batch();
  }
}

/// Hot-reload solver config + batch_interval из PG.
/// Если solver_config_repo_ не задан — fallback на батч-interval из конструктора.
/// При ошибке — log + сохраняем текущее значение (не паника, не падаем).
int MatchingLoop::refresh_batch_interval_ms() {
  if (!solver_config_repo_) {
    return batch_interval_ms_;
  }

  try {
    const auto config = solver_config_repo_->GetActiveConfig();
    const auto configured_ms = static_cast<int>(config.batch_interval.count());
    if (configured_ms > 0) {
      batch_interval_ms_ = configured_ms;
      solver_.SetSolverConfig(config);
      cex::common::log_json(
          "INFO",
          "Updated matching batch interval from solver_config",
          {
              {"batch_interval_ms", std::to_string(batch_interval_ms_)}
      });
    }
  } catch (const std::exception& ex) {
    cex::common::log_json(
        "ERROR",
        "Failed to load active solver_config; using last interval",
        {
            {            "error",                          ex.what()},
            {"batch_interval_ms", std::to_string(batch_interval_ms_)}
    });
  }

  return batch_interval_ms_;
}

// ============================================================================
// on_order_event — handler для orders.normalized Kafka.
//
// Три типа event'ов:
//   create  → FlowOrder.from_proto + index_order + active_[id] = order.
//   cancel  → erase из active_ + index_terminal(filled_qty, "cancelled").
//   amend   → in-place update полей q_max / p_low / p_high / q_rate.
//
// Запись в active_ без mutex'а — single-thread access из consumer thread.
// batch_timer_loop читает active_ для snapshot — потенциальный race,
// но FlowOrder copy безопасен (это просто struct), worst case: solver
// видит stale view одной заявки.
// ============================================================================
void MatchingLoop::on_order_event(
    const fob::orders::v1::OrdersNormalized& evt) {
  if (evt.has_create()) {
    const auto& o = evt.create().order();
    auto fo = domain::FlowOrder::from_proto(o);
    index_order(fo, "pending");
    active_[o.order_id()] = std::move(fo);
    cex::common::log_json(
        "INFO",
        "Order added to matching",
        {
            {"order_id",            o.order_id()},
            {  "symbol", o.instrument().symbol()}
    });
  } else if (evt.has_cancel()) {
    const auto& c = evt.cancel();
    double filled = 0.0;
    {
      auto it = active_.find(c.order_id());
      if (it != active_.end()) {
        filled = static_cast<double>(it->second.filled_cum);
      }
    }
    active_.erase(c.order_id());
    index_terminal(c.order_id(), "cancelled", filled);
    cex::common::log_json("INFO",
                          "Order canceled in matching",
                          {
                              {"order_id", c.order_id()}
    });
  } else if (evt.has_amend()) {
    const auto& a = evt.amend();
    auto it = active_.find(a.order_id());
    if (it != active_.end()) {
      // MVP: only update provided fields if non-zero.
      if (a.has_new_total_qty()) {
        it->second.q_max = cex::common::Decimal::from_proto(a.new_total_qty());
      }
      if (a.has_new_price_low()) {
        it->second.p_low = cex::common::Decimal::from_proto(a.new_price_low());
      }
      if (a.has_new_price_high()) {
        it->second.p_high =
            cex::common::Decimal::from_proto(a.new_price_high());
      }
      if (a.has_new_max_speed()) {
        it->second.q_rate = cex::common::Decimal::from_proto(a.new_max_speed());
      }
      cex::common::log_json("INFO",
                            "Order amended in matching",
                            {
                                {"order_id", a.order_id()}
      });
    }
  }
}

// ============================================================================
// on_liquidity_curve — handler для venue.liquidity.fob Kafka (F-11).
//
// Обновляет planner_inputs_cache_ (under mutex) — это shared state между
// run_one_batch() и этим handler'ом.
// usable=false означает venue health или confidence слишком низкие.
// ============================================================================
void MatchingLoop::on_liquidity_curve(
    const fob::venue::v1::VenueLiquidityCurve& curve) {
  PlannerVenueInput input;
  std::size_t cached_curves = 0;
  std::size_t usable_curves = 0;
  {
    std::lock_guard<std::mutex> lock(planner_inputs_cache_mutex_);
    input = planner_inputs_cache_.UpsertCurve(curve);
    cached_curves = planner_inputs_cache_.CachedCurveCount();
    usable_curves = planner_inputs_cache_.UsableCurveCount();
  }

  if (input.symbol.empty() || input.venue_id.empty()) {
    cex::common::log_json("WARN",
                          "Ignored external liquidity curve without identity",
                          {
                              {"symbol",        input.symbol},
                              { "venue",      input.venue_id},
                              {"reason", input.reject_reason}
    });
    return;
  }

  if (!input.usable) {
    cex::common::log_json(
        "WARN",
        "Rejected external liquidity curve for matching",
        {
            {       "symbol",                  input.symbol},
            {        "venue",                input.venue_id},
            {       "reason",           input.reject_reason},
            {"cached_curves", std::to_string(cached_curves)},
            {"usable_curves", std::to_string(usable_curves)}
    });
    return;
  }

  cex::common::log_json(
      "INFO",
      "Updated external liquidity curve",
      {
          {      "service",                                            "matching"},
          {    "component",                                  "execution_planning"},
          {  "participant",                                  "Execution Planning"},
          {        "stage",                              "consume_external_curve"},
          {        "topic",                                 "venue.liquidity.fob"},
          {       "symbol",                                          input.symbol},
          {        "venue",                                        input.venue_id},
          {     "curve_id",                            CurveIdForLog(input.curve)},
          {  "snapshot_id",                             input.curve.snapshot_id()},
          {        "level",                                   input.curve.level()},
          {   "confidence",              std::to_string(input.curve.confidence())},
          {   "bid_levels", std::to_string(input.curve.bid_curve().q_grid_size())},
          {   "ask_levels", std::to_string(input.curve.ask_curve().q_grid_size())},
          {  "max_bid_qty",  std::to_string(MaxCurveQty(input.curve.bid_curve()))},
          {  "max_ask_qty",  std::to_string(MaxCurveQty(input.curve.ask_curve()))},
          {"cached_curves",                         std::to_string(cached_curves)},
          {"usable_curves",                         std::to_string(usable_curves)},
          {  "source_file",              "cpp/matching/src/app/matching_loop.cpp"}
  });
}

// ============================================================================
// on_venue_health — handler для venue.health Kafka (F-11).
//
// Обновляет venue health state в cache. Может deactivate venue для последующих
// batches. affected_inputs — list curves, чья usability изменилась из-за
// этого health update.
// ============================================================================
void MatchingLoop::on_venue_health(const fob::venue::v1::VenueHealth& health) {
  HealthUpsertResult update;
  std::size_t cached_curves = 0;
  std::size_t usable_curves = 0;
  {
    std::lock_guard<std::mutex> lock(planner_inputs_cache_mutex_);
    update = planner_inputs_cache_.UpsertHealth(health);
    cached_curves = planner_inputs_cache_.CachedCurveCount();
    usable_curves = planner_inputs_cache_.UsableCurveCount();
  }

  if (update.venue.empty()) {
    return;
  }

  if (!update.stored) {
    cex::common::log_json(
        "INFO",
        "Ignored venue health update for matching",
        {
            { "venue",          update.venue},
            {"reason", update.ignored_reason}
    });
    return;
  }

  if (update.venue_usable) {
    cex::common::log_json(
        "INFO",
        "Updated venue health for matching",
        {
            {        "venue",         update.venue                             },
            {       "status", std::to_string(static_cast<int>(health.status()))},
            {      "routing",
             std::to_string(static_cast<int>(health.routing_recommendation())) },
            { "health_score",             std::to_string(health.health_score())},
            {"cached_curves",                     std::to_string(cached_curves)},
            {"usable_curves",                     std::to_string(usable_curves)}
    });
  } else {
    cex::common::log_json(
        "WARN",
        "Disabled venue for matching",
        {
            {        "venue",         update.venue                             },
            {       "reason",                        update.venue_reject_reason},
            {       "status", std::to_string(static_cast<int>(health.status()))},
            {      "routing",
             std::to_string(static_cast<int>(health.routing_recommendation())) },
            { "health_score",             std::to_string(health.health_score())},
            {"cached_curves",                     std::to_string(cached_curves)},
            {"usable_curves",                     std::to_string(usable_curves)}
    });
  }

  for (const auto& input : update.affected_inputs) {
    if (input.usable) {
      continue;
    }
    cex::common::log_json("WARN",
                          "Rejected planner venue input after health update",
                          {
                              {"symbol",        input.symbol},
                              { "venue",      input.venue_id},
                              {"reason", input.reject_reason}
    });
  }
}

/// Filter cached venue curves через венозные health gates.
/// Public wrapper с lock. Используется в legacy paths / tests.
domain::ExternalLiquidityBySymbol MatchingLoop::filtered_external_liquidity()
    const {
  std::lock_guard<std::mutex> lock(planner_inputs_cache_mutex_);
  return filtered_external_liquidity_unlocked();
}

/// Internal version — caller обязан удерживать planner_inputs_cache_mutex_.
/// Также проверяет env override MATCHING_DISABLE_EXTERNAL_VENUES (kill-switch
/// для external routing на случай incident'а).
/// static const local — Meyers singleton pattern, инициализируется один раз
/// при первом вызове (thread-safe в C++11+).
domain::ExternalLiquidityBySymbol
MatchingLoop::filtered_external_liquidity_unlocked() const {
  static const bool external_disabled = []() {
    const auto raw =
        cex::common::Env::try_get_string("MATCHING_DISABLE_EXTERNAL_VENUES");
    if (!raw.has_value()) {
      return false;
    }
    const std::string& v = *raw;
    return v == "1" || v == "true" || v == "TRUE";
  }();
  if (external_disabled) {
    return {};
  }
  return planner_inputs_cache_.LegacyBestProjection();
}

// ============================================================================
// run_one_batch — главный batch cycle. Вызывается batch_timer_loop'ом.
//
// Шаги:
//   1. Generate batch_id (UUID v4) + текущее время.
//   2. Source flow orders: PG (если flow_order_repository_) ИЛИ in-memory active_.
//   3. Fetch reference prices через MarketData (для solver Init initial π).
//   4. Per-order planner forecast (F-11 venue comparisons).
//   5. Filter usable external liquidity по health gates.
//   6. RunBatchUseCase.Execute → BatchResult (solver + cap + publish).
//   7. PR-F02-006 pre-pass: apply batch.order_updates() к order_index_.
//   8. order_index_ sync с active_orders (для UI snapshot).
//   9. Persist fill_deltas в PG через UpdateFilledVolumes (если репо есть).
//  10. Log position snapshots (F-06).
//  11. F-12 multi-venue routing fan-out (PR-F12-15 / DoD-2).
//  12. Publish hedge_execution_intents.
//  13. Build + publish execution intents для external fills.
//  14. Summary log.
//
// Heavy method ~600+ строк — кандидат на decomposition в future refactor.
// ============================================================================
// ============================================================================
// run_grouped_batch — F-09 (T-F09-048). Аддитивный grouped combo-цикл.
//
// Полностью независим от single-leg F-04: gated (grouped_enabled_), вся работа
// в try/catch, при пустом наборе групп — мгновенный выход без накладных.
//   load active groups (PG) → reference prices символов групп (market_data) →
//   SolveGroupedBatch → per группа: build ExecutionGroup → publish (Kafka) →
//   persist (PG, идемпотентно). ADR-033: publish раньше/вместе с persist.
// ============================================================================
void MatchingLoop::run_grouped_batch(const std::string& batch_id) {
  if (!grouped_enabled_ || !active_groups_loader_ || !solve_grouped_uc_ ||
      !eg_producer_.has_value() || !eg_repo_) {
    return;
  }
  try {
    const auto groups = active_groups_loader_->LoadActiveGroups();
    if (groups.empty()) {
      return;  // нет grouped-combo — выходим без накладных (типовой случай)
    }

    // Reference prices для символов групп (синтетические FlowOrder-пробы).
    domain::ReferencePrices reference_prices;
    if (market_data_client_) {
      std::vector<domain::FlowOrder> probes;
      probes.reserve(groups.size());
      for (const auto& g : groups) {
        domain::FlowOrder fo;
        fo.order_id = g.parent_order_id;
        for (const auto& leg : g.legs) {
          domain::FlowOrderLeg fl;
          fl.instrument_symbol = leg.instrument_symbol;
          fl.weight = cex::common::Decimal{1, 0};
          fo.legs.push_back(std::move(fl));
        }
        probes.push_back(std::move(fo));
      }
      try {
        for (const auto& [sym, dec] : market_data_client_->GetReferencePrices(probes)) {
          reference_prices[sym] = cex::common::Decimal::from_proto(dec);
        }
      } catch (const std::exception& ex) {
        cex::common::log_json("WARN", "Grouped batch: reference prices unavailable",
                              {{"batch_id", batch_id}, {"error", ex.what()}});
      }
    }

    // MVP-5 (ADR-037): external-ноги combo исполняются на venue через
    // ExecutionIntent, а не внутренним solver'ом. Split аддитивен — для combo без
    // external-ног (venue_preferences пусто) internal_groups == groups, поведение
    // не меняется.
    std::vector<domain::MultiLegVectorOrder> internal_groups;
    internal_groups.reserve(groups.size());
    std::size_t external_intents = 0;
    {
      infra::ExecutionIntentsProducer ext_producer(producer_);
      for (const auto& g : groups) {
        auto split = SplitInternalExternal(g);
        for (const auto& ext_leg : split.external) {
          const auto intent =
              BuildExternalIntent(g.parent_order_id, batch_id, cex::common::uuid_v4(), ext_leg);
          if (ext_producer.produce(intent)) ++external_intents;
        }
        if (!split.internal.empty()) {
          domain::MultiLegVectorOrder ig = g;
          ig.legs = std::move(split.internal);
          internal_groups.push_back(std::move(ig));
        }
      }
    }
    if (external_intents > 0) {
      cex::common::log_json("INFO", "Combo external legs routed to venues",
                            {{"batch_id", batch_id}, {"intents", std::to_string(external_intents)}});
    }

    const auto results = solve_grouped_uc_->Execute(internal_groups, reference_prices);

    std::size_t produced = 0;
    for (const auto& r : results) {
      const domain::MultiLegVectorOrder* order = nullptr;
      for (const auto& g : internal_groups) {
        if (g.parent_order_id == r.parent_order_id) order = &g;
      }
      if (order == nullptr) continue;
      // Ничего не исполнено в этом batch (blocked / G=0) → не плодим пустые
      // ExecutionGroup. Группа остаётся активной и попробует на след. цикле.
      if (r.solve.leg_execs.empty()) continue;

      infra::ExecutionGroupRecord rec;
      rec.execution_group_id = cex::common::uuid_v4();
      rec.batch_id = batch_id;
      rec.order = *order;
      rec.result = r.solve;
      rec.reference_prices = reference_prices;

      // Один proto и в Kafka, и в PG (ADR-033: publish перед persist).
      const auto eg = infra::BuildExecutionGroup(rec);
      eg_producer_->ProduceBuilt(eg);
      try {
        eg_repo_->PersistExecutionGroup(eg);
      } catch (const std::exception& ex) {
        cex::common::log_json("ERROR", "Grouped batch: persist failed",
                              {{"batch_id", batch_id},
                               {"parent_order_id", r.parent_order_id},
                               {"error", ex.what()}});
      }

      // MVP-4 (ADR-038): OCO/bracket leg-переходы после исполнения (filled_cum
      // обновлён PersistExecutionGroup). Аддитивно; группы без графа → no-op.
      if (child_graph_repo_) {
        try {
          auto state = child_graph_repo_->LoadComboGroupState(r.parent_order_id);
          if (!state.edges.empty()) {
            auto transitions = domain::ApplyOCOTransitions(state);
            auto bracket = domain::ResizeBracketExits(state);
            transitions.insert(transitions.end(), bracket.begin(), bracket.end());
            // MVP-4.1: conditional-активация по триггеру (рыночные цены batch).
            auto conditional = domain::ApplyConditionalActivations(state, reference_prices);
            transitions.insert(transitions.end(), conditional.begin(), conditional.end());
            if (!transitions.empty()) {
              child_graph_repo_->PersistChildGraphTransitions(rec.execution_group_id, batch_id,
                                                              state, transitions);
              cex::common::log_json("INFO", "Child-graph transitions applied",
                                    {{"batch_id", batch_id},
                                     {"parent_order_id", r.parent_order_id},
                                     {"transitions", std::to_string(transitions.size())}});
            }
          }
        } catch (const std::exception& ex) {
          cex::common::log_json("ERROR", "Child-graph step failed",
                                {{"batch_id", batch_id},
                                 {"parent_order_id", r.parent_order_id},
                                 {"error", ex.what()}});
        }
      }

      ++produced;
    }

    cex::common::log_json("INFO", "Grouped batch executed",
                          {{"batch_id", batch_id},
                           {"groups", std::to_string(groups.size())},
                           {"execution_groups", std::to_string(produced)}});
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Grouped batch failed (single-leg unaffected)",
                          {{"batch_id", batch_id}, {"error", ex.what()}});
  }
}

void MatchingLoop::run_one_batch() {
  using namespace std::chrono;

  const auto cycle_started_at = steady_clock::now();
  const auto batch_time = std::chrono::system_clock::now();
  const auto batch_id = cex::common::uuid_v4();

  // F-09 (T-F09-048): grouped combo-цикл — аддитивный шаг до single-leg F-04.
  // Независим, gated и в try/catch внутри; на single-leg не влияет.
  run_grouped_batch(batch_id);

  infra::ExecutionIntentsProducer execution_intents_producer(producer_);
  std::vector<fob::execution::v1::ExecutionIntent> pending_execution_intents;

  std::unordered_map<std::string, domain::FlowOrder> repo_active_orders;
  auto* active_orders = &active_;

  if (flow_order_repository_) {
    try {
      const auto loaded_orders =
          flow_order_repository_->LoadActiveFlowOrders(batch_time);
      repo_active_orders = MapRepoOrders(loaded_orders);
      active_orders = &repo_active_orders;
      cex::common::log_json(
          "INFO",
          "Loaded active flow orders from repository",
          {
              {"batch_id",                                  batch_id},
              {   "count", std::to_string(repo_active_orders.size())}
      });
    } catch (const std::exception& ex) {
      cex::common::log_json(
          "ERROR",
          "Failed to load active flow orders from repository",
          {
              {           "batch_id",batch_id                                     },
              {              "error", ex.what()},
              {"batch_cycle_time_ms",
               std::to_string(duration_cast<milliseconds>(steady_clock::now() -
               cycle_started_at)
               .count())                       }
      });
      return;
    }
  }

  std::unordered_map<std::string, fob::common::v1::Decimal> reference_prices;
  if (market_data_client_ && !active_orders->empty()) {
    cex::common::log_json(
        "INFO",
        "Batch fetching reference prices",
        {
            {"batch_id",                              batch_id},
            {  "orders", std::to_string(active_orders->size())}
    });
    try {
      reference_prices = market_data_client_->GetReferencePrices(
          SnapshotActiveOrders(*active_orders));
    } catch (const std::exception& ex) {
      cex::common::log_json("WARN",
                            "Failed to fetch reference prices for batch",
                            {
                                {"batch_id",  batch_id},
                                {   "error", ex.what()}
      });
    }
    cex::common::log_json(
        "INFO",
        "Batch reference prices done",
        {
            {"batch_id",                                batch_id},
            {     "got", std::to_string(reference_prices.size())}
    });
  }

  const double batch_interval_sec =
      static_cast<double>(batch_interval_ms_) / 1000.0;
  std::size_t intents_attempted = 0;
  std::size_t intents_published = 0;
  std::size_t hedge_execution_intents_attempted = 0;
  std::size_t hedge_execution_intents_published = 0;
  std::size_t hedge_execution_intents_deduped = 0;
  for (const auto& [order_id, order] : *active_orders) {
    (void)order_id;

    const double remaining_qty =
        std::max(0.0, static_cast<double>(order.remaining_qty()));
    const double max_speed = std::max(0.0, static_cast<double>(order.q_rate));
    const double target_qty =
        std::min(remaining_qty, max_speed * batch_interval_sec);
    if (target_qty <= 0.0 || !std::isfinite(target_qty)) {
      continue;
    }

    OrderForecastRequest forecast_request;
    forecast_request.order_id = order.order_id;
    forecast_request.target_qty = target_qty;
    forecast_request.target_speed = max_speed;
    const auto price_limits = NormalizeOrderPriceLimits(order);
    forecast_request.buy_limit_price = price_limits.buy_limit_price;
    forecast_request.sell_limit_price = price_limits.sell_limit_price;

    for (const auto& leg : order.legs) {
      const double coefficient = static_cast<double>(leg.weight);
      if (!std::isfinite(coefficient) || std::abs(coefficient) <= 1e-12) {
        continue;
      }

      OrderLegRequest leg_request;
      leg_request.symbol = leg.instrument_symbol;
      leg_request.coefficient = coefficient;
      forecast_request.legs.push_back(std::move(leg_request));

      const auto reference_it = reference_prices.find(leg.instrument_symbol);
      if (reference_it != reference_prices.end()) {
        const double reference_price = static_cast<double>(
            cex::common::Decimal::from_proto(reference_it->second));
        if (std::isfinite(reference_price) && reference_price > 0.0) {
          forecast_request.reference_prices_by_symbol[leg.instrument_symbol] =
              reference_price;
        }
      }
    }

    OrderForecast order_forecast;
    {
      std::lock_guard<std::mutex> lock(planner_inputs_cache_mutex_);
      order_forecast =
          planner_inputs_cache_.BuildOrderForecast(forecast_request);
    }
    for (const auto& leg_forecast : order_forecast.leg_forecasts) {
      const auto feasible_count = static_cast<std::size_t>(
          std::count_if(leg_forecast.comparisons.begin(),
                        leg_forecast.comparisons.end(),
                        [](const VenueComparison& comparison) {
                          return comparison.feasible;
                        }));

      std::string best_venue;
      std::string expected_vwap{"0"};
      std::string expected_is_bps{"0"};
      std::string reason = leg_forecast.reject_reason;
      if (leg_forecast.best_comparison.has_value()) {
        best_venue = leg_forecast.best_comparison->venue_id;
        expected_vwap =
            std::to_string(leg_forecast.best_comparison->expected_vwap);
        expected_is_bps =
            std::to_string(leg_forecast.best_comparison->expected_is_bps);
        if (reason.empty()) {
          reason = leg_forecast.best_comparison->reject_reason;
        }
      }

      cex::common::log_json(
          "INFO",
          "Planner venue comparisons for order leg",
          {
              {        "service",                               "matching"},
              {      "component",                     "execution_planning"},
              {    "participant",                     "Execution Planning"},
              {          "stage",                 "compare_venues_for_leg"},
              {       "order_id",                           order.order_id},
              {         "symbol",                      leg_forecast.symbol},
              {           "side",  ForecastSideToString(leg_forecast.side)},
              { "feasible_count",           std::to_string(feasible_count)},
              {     "best_venue",                               best_venue},
              {  "expected_vwap",                            expected_vwap},
              {"expected_is_bps",                          expected_is_bps},
              {       "feasible", leg_forecast.feasible ? "true" : "false"},
              {         "reason",                                   reason},
              {    "source_file", "cpp/matching/src/app/matching_loop.cpp"}
      });
    }

    if (!order_forecast.feasible) {
      cex::common::log_json("INFO",
                            "Order forecast infeasible for external planner",
                            {
                                {"batch_id",                     batch_id},
                                {"order_id",               order.order_id},
                                {  "reason", order_forecast.reject_reason}
      });
    }
  }

  std::size_t cached_curves = 0;
  std::size_t usable_curves = 0;
  domain::ExternalLiquidityBySymbol usable_external_liquidity;
  std::unordered_map<std::string, std::size_t> compared_venue_counts;
  {
    std::lock_guard<std::mutex> lock(planner_inputs_cache_mutex_);
    cached_curves = planner_inputs_cache_.CachedCurveCount();
    usable_curves = planner_inputs_cache_.UsableCurveCount();
    usable_external_liquidity = filtered_external_liquidity_unlocked();
    for (const auto& [symbol, curve] : usable_external_liquidity) {
      (void)curve;
      compared_venue_counts[symbol] =
          planner_inputs_cache_.GetPlannerInputsSnapshotForSymbol(symbol)
              .size();
    }
  }
  if (usable_curves != cached_curves) {
    cex::common::log_json(
        "INFO",
        "Filtered external venues for batch",
        {
            {        "batch_id",batch_id                                },
            {   "cached_curves", std::to_string(cached_curves)},
            {   "usable_curves", std::to_string(usable_curves)},
            {"selected_symbols",
             std::to_string(usable_external_liquidity.size()) }
    });
  }
  for (const auto& [symbol, curve] : usable_external_liquidity) {
    cex::common::log_json(
        "INFO",
        "Selected best external venue for batch symbol",
        {
            {         "batch_id",      batch_id                                 },
            {           "symbol",                                         symbol},
            {            "venue",                               curve.venue_id()},
            {      "snapshot_id",                            curve.snapshot_id()},
            {         "curve_id",                           CurveIdForLog(curve)},
            {"considered_venues",
             std::to_string(compared_venue_counts[symbol])                      },
            {      "max_bid_qty", std::to_string(MaxCurveQty(curve.bid_curve()))},
            {      "max_ask_qty", std::to_string(MaxCurveQty(curve.ask_curve()))}
    });
  }

  std::unordered_map<std::string, double> total_qty_before;
  for (const auto& [oid, o] : *active_orders) {
    total_qty_before[oid] = static_cast<double>(o.q_max);
  }

  const auto batch_result = run_batch_uc_.Execute(
      batch_id, *active_orders, reference_prices, usable_external_liquidity);
  const auto batch_cycle_time_ms =
      duration_cast<milliseconds>(steady_clock::now() - cycle_started_at)
          .count();

  {
    std::lock_guard<std::mutex> lock(order_index_mutex_);
    // PR-F02-006: apply terminal OrderUpdates from THIS batch's solver
    // output BEFORE the active_orders refresh below. The previous
    // PR-F02-003 loop reads `snap.filled_qty` / `snap.status` from
    // `active_orders`, but run_batch_uc.cpp:357-371 erases entries that
    // hit a terminal state (FILLED/CANCELLED/EXPIRED) DURING this batch.
    // By the time we get here, the entry that just became "filled" is
    // gone from active_orders, so the existing in_active branch can't
    // pick it up and the else-if guard (snap.filled_qty >= total_qty)
    // sees the value from one batch ago and refuses to promote. Reading
    // the proto OrderUpdates directly captures the just-set
    // filled_qty_total and status, so /orders/<id> reports filled with
    // the correct full quantity instead of stuck at "partial 0.004/0.005"
    // as seen on the front-end ("Частично" badge on a fully-filled BUY).
    for (const auto& update : batch_result.batch.order_updates()) {
      auto it = order_index_.find(update.order_id());
      if (it == order_index_.end()) continue;
      auto& snap = it->second;
      if (update.has_filled_qty_total()) {
        snap.filled_qty = static_cast<double>(
            cex::common::Decimal::from_proto(update.filled_qty_total()));
      }
      switch (update.status()) {
        case fob::common::v1::ORDER_STATUS_FILLED:
          snap.status = "filled";
          // Defensive: if the proto carried no filled_qty_total field
          // for some reason but reported FILLED, mirror total_qty so the
          // UI doesn't show "filled 0/qty".
          if (snap.total_qty > 0.0 && snap.filled_qty < snap.total_qty) {
            snap.filled_qty = snap.total_qty;
          }
          break;
        case fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED:
          snap.status = "partial";
          break;
        case fob::common::v1::ORDER_STATUS_CANCELED:
          snap.status = "cancelled";
          break;
        case fob::common::v1::ORDER_STATUS_EXPIRED:
          snap.status = "expired";
          break;
        case fob::common::v1::ORDER_STATUS_REJECTED:
          snap.status = "rejected";
          break;
        default:
          break;
      }
    }

    for (auto& [oid, snap] : order_index_) {
      auto it = active_orders->find(oid);
      if (it != active_orders->end()) {
        // Order is present in this batch's active set — sync snap with
        // authoritative domain status from PG/in-memory active_orders.
        // PR-F02-003: must cover ALL incoming statuses (was kPartiallyFilled
        // / kCancelled / kExpired only) so a stale snap.status="filled"
        // from a prior racy batch — when the order momentarily fell out of
        // active_orders during a PG load transition — resets back to the
        // correct value when the order reappears. Without the kActive/kNew
        // and kFilled branches the snapshot stays "filled, filled_qty=0"
        // forever and the user-facing trade list shows "Завершено / 0/qty".
        snap.filled_qty = static_cast<double>(it->second.filled_cum);
        switch (it->second.status) {
          case domain::FlowOrderStatus::kPartiallyFilled:
            snap.status = "partial";
            break;
          case domain::FlowOrderStatus::kCancelled:
            snap.status = "cancelled";
            break;
          case domain::FlowOrderStatus::kExpired:
            snap.status = "expired";
            break;
          case domain::FlowOrderStatus::kFilled:
            snap.status = "filled";
            break;
          case domain::FlowOrderStatus::kActive:
          case domain::FlowOrderStatus::kNew:
            snap.status = "pending";
            break;
          case domain::FlowOrderStatus::kLiquidated:
          default:
            // leave snap.status untouched for statuses we don't have
            // explicit UI mapping for (caller will treat unknown as terminal).
            break;
        }
      } else if (snap.status == "pending" || snap.status == "partial") {
        // PR-F02-003: previously this branch auto-promoted any tracked
        // order missing from active_orders to status="filled",
        // filled_qty=total_qty. That's wrong: an order can drop out of
        // active_orders for many non-fill reasons (window expired, IOC
        // excluded by query, transient PG load failure, order_index_
        // populated from Kafka before PG row caught up). Only promote to
        // "filled" when the snap's accumulated filled_qty actually reached
        // total_qty; otherwise leave the prior state for the next batch
        // to confirm (the order is likely to come back when active_orders
        // refreshes).
        if (snap.total_qty > 0.0 && snap.filled_qty >= snap.total_qty) {
          snap.filled_qty = snap.total_qty;
          snap.status = "filled";
        }
      }
    }
  }

  if (batch_result.status == RunBatchStatus::kSkippedEmpty) {
    cex::common::log_json(
        "INFO",
        "Skipped empty batch",
        {
            {           "batch_id",               batch_result.batch_id},
            {"batch_cycle_time_ms", std::to_string(batch_cycle_time_ms)}
    });
    return;
  }

  if (batch_result.status == RunBatchStatus::kFailedSolver) {
    metrics_.ObserveError("solve_failed");
    cex::common::log_json(
        "ERROR",
        "RunBatch solver failed",
        {
            {           "batch_id",                      batch_result.batch_id},
            {      "active_before", std::to_string(batch_result.active_before)},
            {"batch_cycle_time_ms",        std::to_string(batch_cycle_time_ms)}
    });
    return;
  }

  if (batch_result.status == RunBatchStatus::kFailedPublish) {
    metrics_.ObserveError("publish_failed");
    cex::common::log_json(
        "ERROR",
        "RunBatch publish failed",
        {
            {           "batch_id",                      batch_result.batch_id},
            {              "fills",         std::to_string(batch_result.fills)},
            {      "solve_time_ms", std::to_string(batch_result.solve_time_ms)},
            {"batch_cycle_time_ms",        std::to_string(batch_cycle_time_ms)}
    });
    return;
  }

  if (flow_order_repository_ && !batch_result.fill_deltas.empty()) {
    try {
      flow_order_repository_->UpdateFilledVolumes(batch_result.fill_deltas,
                                                  batch_time);
    } catch (const std::exception& ex) {
      metrics_.ObserveError("persist_failed");
      cex::common::log_json(
          "ERROR",
          "Failed to persist flow order fills",
          {
              {   "batch_id",batch_result.batch_id                             },
              {      "error",                        ex.what()},
              {"fills_count",
               std::to_string(batch_result.fill_deltas.size())}
      });
    }
  }

  for (const auto& snapshot : batch_result.position_snapshots) {
    cex::common::log_json(
        "INFO",
        "Calculated provider position snapshot",
        {
            {    "batch_id",                                      snapshot.batch_id},
            {"provider_id",                                   snapshot.provider_id},
            {     "symbol",                                        snapshot.symbol},
            {    "net_qty",                               snapshot.net_qty.to_string()},
            {"clearing_price",
             cex::common::Decimal::from_proto(snapshot.clearing_price).to_string()}
    });
  }

  cex::common::log_json(
      "INFO",
      "Built auto-hedge execution intents from hedge trigger decisions",
      {
          {"batch_id", batch_id},
          {"hedge_trigger_decisions",
           std::to_string(batch_result.hedge_trigger_decisions.size())},
          {"hedge_execution_intents",
           std::to_string(batch_result.hedge_execution_intents.size())}
  });

  // F-12 DoD-2 (PR-F12-15): multi-venue routing fan-out.
  // For each pre-fan-out intent, ask BuildMultiVenuePlan to split
  // target_qty across allowed venues proportionally to L(v). If the
  // plan yields >=2 allocations we replace the single intent with N
  // child clones (each gets its own hedge_flow_id, venue, qty share).
  // No-op fallback: when only 1 venue passes filtering or planner
  // returns infeasible, we keep the original intent unchanged.
  std::vector<fob::execution::v1::ExecutionIntent> fanned_out;
  fanned_out.reserve(batch_result.hedge_execution_intents.size());
  std::size_t fanout_expansions = 0;
  for (const auto& intent : batch_result.hedge_execution_intents) {
    PlanRequest plan_req;
    plan_req.symbol = intent.instrument().symbol();
    plan_req.side = intent.side();
    plan_req.target_qty = cex::common::Decimal::from_proto(intent.target_qty());
    for (const auto& v : intent.allowed_venues()) plan_req.allowed_venues.push_back(v);
    {
      std::lock_guard<std::mutex> lock(planner_inputs_cache_mutex_);
      plan_req.planner_inputs =
          planner_inputs_cache_.GetPlannerInputsSnapshotForSymbol(
              intent.instrument().symbol());
    }
    const auto plan = BuildMultiVenuePlan(plan_req);

    // F-12 DoD-2 diagnostic: always log the plan outcome so operators
    // can see why a fan-out did or didn't happen (candidate count is
    // the usual surprise — health/liquidity filters narrow venues).
    {
      std::string venue_list;
      for (const auto& a : plan.allocations) {
        if (!venue_list.empty()) venue_list += ",";
        venue_list += a.venue_id + ":" + a.qty.to_string();
      }
      cex::common::log_json(
          "INFO", "Multi-venue routing plan computed",
          {{"batch_id", batch_id},
           {"hedge_flow_id", intent.hedge_flow_id()},
           {"symbol", intent.instrument().symbol()},
           {"planner_inputs", std::to_string(plan_req.planner_inputs.size())},
           {"allowed_venues_count", std::to_string(plan_req.allowed_venues.size())},
           {"allocations", std::to_string(plan.allocations.size())},
           {"feasible", plan.feasible ? "true" : "false"},
           {"reject_reason", plan.reject_reason},
           {"venue_split", venue_list}});
    }

    if (plan.feasible && plan.allocations.size() >= 2) {
      auto children = FanOutIntentByPlan(intent, plan);
      ++fanout_expansions;
      cex::common::log_json(
          "INFO",
          "Multi-venue routing plan expanded hedge intent",
          {{"batch_id", batch_id},
           {"hedge_flow_id", intent.hedge_flow_id()},
           {"symbol", intent.instrument().symbol()},
           {"allocations", std::to_string(plan.allocations.size())},
           {"target_qty", plan_req.target_qty.to_string()}});
      for (auto& child : children) fanned_out.push_back(std::move(child));
    } else {
      // 0 or 1 allocation — keep original.
      fanned_out.push_back(intent);
      if (!plan.feasible) {
        cex::common::log_json(
            "WARN",
            "Multi-venue routing plan not feasible; emitting original intent",
            {{"batch_id", batch_id},
             {"hedge_flow_id", intent.hedge_flow_id()},
             {"reject_reason", plan.reject_reason}});
      }
    }
  }
  if (fanout_expansions > 0) {
    cex::common::log_json(
        "INFO",
        "Multi-venue routing summary",
        {{"batch_id", batch_id},
         {"input_intents",
          std::to_string(batch_result.hedge_execution_intents.size())},
         {"expanded_intents", std::to_string(fanout_expansions)},
         {"output_intents", std::to_string(fanned_out.size())}});
  }

  const auto hedge_publish_result = PublishAutoHedgeExecutionIntents(
      batch_id,
      fanned_out,
      execution_intents_producer);
  hedge_execution_intents_attempted = hedge_publish_result.attempted;
  hedge_execution_intents_published = hedge_publish_result.published;
  hedge_execution_intents_deduped = hedge_publish_result.deduped;
  if (!hedge_publish_result.success) {
    metrics_.ObserveError("execution_intents_publish_failed");
    cex::common::log_json(
        "ERROR",
        "Failed to publish some auto-hedge execution intents (partial failure)",
        {
            {                        "batch_id",                             batch_id},
            {"hedge_execution_intents_attempted",
             std::to_string(hedge_execution_intents_attempted)                       },
            {"hedge_execution_intents_published",
             std::to_string(hedge_execution_intents_published)                       },
            {"hedge_execution_intents_deduped",
             std::to_string(hedge_execution_intents_deduped)}
    });
  }

  auto intent_build_result =
      execution_intent_builder_.BuildFromExternalFills(batch_result.batch);
  pending_execution_intents = std::move(intent_build_result.intents);
  intents_attempted = pending_execution_intents.size();

  for (const auto& intent : pending_execution_intents) {
    cex::common::log_json(
        "INFO",
        "Built execution intent from external fill",
        {
            {     "service",          "matching"                            },
            {   "component",                            "execution_planning"},
            { "participant",                            "Execution Planning"},
            {       "stage",                        "build_execution_intent"},
            {    "batch_id",                                        batch_id},
            {   "intent_id",                              intent.intent_id()},
            {    "order_id",                      intent.internal_order_id()},
            {      "symbol",                    intent.instrument().symbol()},
            {       "venue",                                  intent.venue()},
            {        "side", std::to_string(static_cast<int>(intent.side()))},
            {  "target_qty",
             intent.has_target_qty()
             ? cex::common::Decimal::from_proto(intent.target_qty())
             .to_string()
             : "0"                                                          },
            { "limit_price",
             intent.has_limit_price()
             ? cex::common::Decimal::from_proto(intent.limit_price())
             .to_string()
             : "0"                                                          },
            {"venue_symbol",                           intent.venue_symbol()},
            { "source_file",        "cpp/matching/src/app/matching_loop.cpp"}
    });
  }

  {
    static const bool external_disabled_intents = []() {
      const auto raw =
          cex::common::Env::try_get_string("MATCHING_DISABLE_EXTERNAL_VENUES");
      if (!raw.has_value()) {
        return false;
      }
      const std::string& v = *raw;
      return v == "1" || v == "true" || v == "TRUE";
    }();
    if (external_disabled_intents && !pending_execution_intents.empty()) {
      cex::common::log_json(
          "INFO",
          "Dropping execution intents (external venues disabled)",
          {
              {"batch_id",                                         batch_id},
              {   "count", std::to_string(pending_execution_intents.size())}
      });
      pending_execution_intents.clear();
    }
  }

  if (!pending_execution_intents.empty()) {
    if (!execution_intents_producer.produce(pending_execution_intents)) {
      metrics_.ObserveError("execution_intents_publish_failed");
      cex::common::log_json(
          "ERROR",
          "Failed to publish execution intents",
          {
              {"batch_id",                                         batch_id},
              {   "count", std::to_string(pending_execution_intents.size())}
      });
    } else {
      intents_published = pending_execution_intents.size();
    }
  }

  cex::common::log_json(
      "INFO",
      "Produced batch.outputs",
      {
          {                    "service",                                 "matching"},
          {                  "component",                         "matching_backend"},
          {                "participant",                         "Matching Backend"},
          {                      "stage",                    "publish_batch_outputs"},
          {                      "topic",                            "batch.outputs"},
          {                   "batch_id",                      batch_result.batch_id},
          {                      "fills",         std::to_string(batch_result.fills)},
          {              "active_before", std::to_string(batch_result.active_before)},
          {               "active_after",  std::to_string(batch_result.active_after)},
          {"execution_intents_attempted",          std::to_string(intents_attempted)},
          {"execution_intents_published",          std::to_string(intents_published)},
          {"hedge_execution_intents_attempted",
           std::to_string(hedge_execution_intents_attempted)},
          {"hedge_execution_intents_published",
           std::to_string(hedge_execution_intents_published)},
          {"hedge_execution_intents_deduped",
           std::to_string(hedge_execution_intents_deduped)},
          {"hedge_execution_intents",
           std::to_string(batch_result.hedge_execution_intents.size())},
          {         "position_snapshots",
           std::to_string(batch_result.position_snapshots.size())                },
          {              "solve_time_ms", std::to_string(batch_result.solve_time_ms)},
          {        "batch_cycle_time_ms",        std::to_string(batch_cycle_time_ms)},
          {                "source_file",   "cpp/matching/src/app/matching_loop.cpp"}
  });
}

/// Тонкая обёртка для Kafka publish — делегирует BatchOutputsProducer.
/// Закомментированный код выше — legacy direct-call вариант.
bool MatchingLoop::publish_batch(const fob::matching::v1::BatchResult& batch) {
  return infra::BatchOutputsProducer(producer_).produce(batch);
  // return producer_.produce("batch.outputs", batch.batch_id(),
  // cex::common::to_bytes(batch));
}

// ============================================================================
// index_order / index_terminal / snapshot_order — UI-friendly snapshot
// поддержка для /orders/<id> endpoint.
//
// order_index_ — отдельная map от active_, оптимизированная для read API
// (включает terminal заявки которые уже не в active_). Обновляется
// inline в нескольких местах: on_order_event (create/cancel), run_one_batch
// (после batch_result через PR-F02-006 pre-pass), и через index_terminal.
// ============================================================================

/// Add/update OrderSnapshot для заявки. Status — UI-facing string
/// ("pending"/"partial"/"filled"/"cancelled"/...).
void MatchingLoop::index_order(const domain::FlowOrder& order,
                               const std::string& status) {
  std::lock_guard<std::mutex> lock(order_index_mutex_);
  auto& snap = order_index_[order.order_id];
  snap.order_id = order.order_id;
  snap.status = status;
  snap.total_qty = static_cast<double>(order.q_max);
  snap.filled_qty = static_cast<double>(order.filled_cum);
}

/// Mark заявку как terminal (cancelled/expired/...) с финальным filled_qty.
/// No-op если заявка не была в index_ (мы не отслеживаем заявки которые
/// никогда не приходили через on_order_event).
void MatchingLoop::index_terminal(const std::string& order_id,
                                  const std::string& status,
                                  double filled_qty) {
  std::lock_guard<std::mutex> lock(order_index_mutex_);
  auto it = order_index_.find(order_id);
  if (it == order_index_.end()) {
    return;
  }
  it->second.status = status;
  it->second.filled_qty = filled_qty;
}

/// Lookup snapshot заявки по order_id. std::nullopt если не найдена.
/// Используется HTTP /orders/<id> endpoint для отдачи UI-friendly data.
std::optional<MatchingLoop::OrderSnapshot> MatchingLoop::snapshot_order(
    const std::string& order_id) const {
  std::lock_guard<std::mutex> lock(order_index_mutex_);
  auto it = order_index_.find(order_id);
  if (it == order_index_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace cex::matching::app
