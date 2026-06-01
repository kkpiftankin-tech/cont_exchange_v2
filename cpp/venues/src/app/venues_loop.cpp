#include "app/venues_loop.hpp"

#include "app/sim_config_applier.hpp"

#include <algorithm>
#include <functional>
#include <thread>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>

#include "cex/common/decimal.hpp"
#include "cex/common/env.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

#include "fob/marketdata/v1/marketdata_raw.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "infra/cex_ws_rest_adapter.hpp"
#include "infra/dex_amm_rpc_adapter.hpp"
#include "infra/simulated_venue_adapter.hpp"
#include "infra/venue_sim_adapter.hpp"

namespace cex::venues::app {

using cex::common::Decimal;

namespace {
// F-20 — symbol used for sim-routing scope match; same canonical form the
// SimExecutionAssembler uses (symbol, else base/quote).
std::string RoutingSymbol(const fob::common::v1::Instrument& i) {
  if (!i.symbol().empty()) return i.symbol();
  if (!i.base().empty() && !i.quote().empty()) return i.base() + "/" + i.quote();
  return i.symbol();
}
}  // namespace

namespace {

fob::common::v1::Instrument make_default_instrument() {
  fob::common::v1::Instrument instrument;
  instrument.set_symbol("BTC/USDT");
  instrument.set_base("BTC");
  instrument.set_quote("USDT");
  return instrument;
}

domain::VenueSubscription make_default_subscription() {
  domain::VenueSubscription sub;
  sub.instrument = make_default_instrument();
  sub.venue_symbol = "BTCUSDT";
  sub.channels = {
      domain::VenueFeedChannel::kOrderBook,
      domain::VenueFeedChannel::kTrades,
      domain::VenueFeedChannel::kTicker,
      domain::VenueFeedChannel::kStatus,
      domain::VenueFeedChannel::kPoolState,
  };
  sub.depth_levels = 20;
  return sub;
}

domain::VenueSnapshotRequest make_default_snapshot_request() {
  domain::VenueSnapshotRequest req;
  req.instrument = make_default_instrument();
  req.venue_symbol = "BTCUSDT";
  req.depth_levels = 20;
  return req;
}

double env_double(const std::string& key, const double def) {
  const std::string text = cex::common::Env::get_string(key, "");
  if (text.empty()) return def;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == nullptr || *end != '\0') return def;
  return value;
}

bool env_bool(const std::string& key, const bool def) {
  std::string text = cex::common::Env::get_string(key, "");
  if (text.empty()) return def;
  std::transform(text.begin(), text.end(), text.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (text == "1" || text == "true" || text == "yes" || text == "on") return true;
  if (text == "0" || text == "false" || text == "no" || text == "off") return false;
  return def;
}

Decimal env_decimal_units_scale(const std::string& units_key,
                                const std::string& scale_key,
                                const Decimal def) {
  int64_t units = def.units;
  const std::string units_text = cex::common::Env::get_string(units_key, "");
  if (!units_text.empty()) {
    char* end = nullptr;
    const long long parsed = std::strtoll(units_text.c_str(), &end, 10);
    if (end != nullptr && *end == '\0') {
      units = static_cast<int64_t>(parsed);
    }
  }
  const int scale = cex::common::Env::get_int(scale_key, def.scale);
  return Decimal{
      .units = std::max<int64_t>(0, units),
      .scale = std::max(0, scale),
  };
}

infra::DexSyncMode env_dex_sync_mode() {
  const std::string mode = cex::common::Env::get_string("DEX_SYNC_MODE", "hybrid");
  if (mode == "polling") return infra::DexSyncMode::kPolling;
  if (mode == "subscription") return infra::DexSyncMode::kSubscription;
  return infra::DexSyncMode::kHybrid;
}

infra::DexFinalizationClass env_dex_finalization_class() {
  const std::string cls = cex::common::Env::get_string("DEX_FINALIZATION_CLASS", "fast");
  if (cls == "slow") return infra::DexFinalizationClass::kSlow;
  return infra::DexFinalizationClass::kFast;
}

int64_t now_epoch_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

fob::execution::v1::ExecutionReport make_execution_exception_report(
    const fob::execution::v1::ExecutionIntent& intent,
    const std::string& code,
    const std::string& message) {
  fob::execution::v1::ExecutionReport report;

  auto* meta = report.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("venues");
  meta->set_correlation_id(
      intent.meta().correlation_id().empty()
          ? intent.intent_id()
          : intent.meta().correlation_id());
  meta->set_partition_key(intent.intent_id());

  report.set_report_id(cex::common::uuid_v4());
  report.set_intent_id(intent.intent_id());
  report.set_venue(intent.venue());
  *report.mutable_instrument() = intent.instrument();
  report.set_venue_symbol(intent.venue_symbol());
  report.set_client_order_id(intent.client_order_id());
  report.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED);
  *report.mutable_remaining_qty() = intent.target_qty();
  report.mutable_error()->set_code(code);
  report.mutable_error()->set_message(message);
  return report;
}

L3ImpactModel env_l3_impact_model() {
  std::string model = cex::common::Env::get_string("CURVE_L3_IMPACT_MODEL", "linear");
  std::transform(model.begin(), model.end(), model.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (model == "quadratic") return L3ImpactModel::kQuadratic;
  if (model == "sqrt") return L3ImpactModel::kSqrt;
  return L3ImpactModel::kLinear;
}

std::string partition_key_for(const domain::VenueRawSnapshot& snapshot) {
  return snapshot.venue_id + "|" + snapshot.instrument.symbol();
}

std::string normalize_routing_mode(const std::string& raw_mode) {
  std::string mode = raw_mode;
  std::transform(mode.begin(), mode.end(), mode.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return mode == "watch" ? "watch" : "auto";
}

std::string normalize_curve_level(const std::string& raw_level) {
  std::string level = raw_level;
  std::transform(level.begin(), level.end(), level.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (level == "L1" || level == "L2" || level == "L3" || level == "OFF") {
    return level;
  }
  return "L3";
}

std::size_t approx_snapshot_payload_bytes(const domain::VenueRawSnapshot& snapshot) {
  const std::size_t levels = snapshot.bids.size() + snapshot.asks.size();
  return levels * (sizeof(int64_t) * 2) +
      snapshot.instrument.symbol().size() +
      snapshot.venue_symbol.size();
}

void publish_raw_order_book(cex::common::KafkaProducer* producer,
                            const domain::VenueRawSnapshot& snapshot) {
  fob::marketdata::v1::MarketDataRaw evt;
  auto* meta = evt.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("venues");
  meta->set_correlation_id(cex::common::uuid_v4());
  meta->set_partition_key(partition_key_for(snapshot));

  auto* orderbook = evt.mutable_orderbook();
  orderbook->set_venue(snapshot.venue_id);
  *orderbook->mutable_instrument() = snapshot.instrument;
  *orderbook->mutable_timestamp() = snapshot.timestamp;
  orderbook->set_nonce(snapshot.sequence);

  for (const auto& bid : snapshot.bids) {
    auto* level = orderbook->add_bids();
    *level->mutable_price() = bid.price.to_proto();
    *level->mutable_amount() = bid.qty.to_proto();
  }
  for (const auto& ask : snapshot.asks) {
    auto* level = orderbook->add_asks();
    *level->mutable_price() = ask.price.to_proto();
    *level->mutable_amount() = ask.qty.to_proto();
  }

  const std::string payload = cex::common::to_bytes(evt);
  const bool ok = producer->produce("marketdata.raw", meta->partition_key(), payload);
  cex::common::log_json(ok ? "INFO" : "ERROR", "Published marketdata.raw order_book",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "publish_raw_order_book"},
                         {"topic", "marketdata.raw"},
                         {"event_id", meta->event_id()},
                         {"venue", snapshot.venue_id},
                         {"symbol", snapshot.instrument.symbol()},
                         {"venue_symbol", snapshot.venue_symbol},
                         {"status", domain::ToString(snapshot.status)},
                         {"sequence", std::to_string(snapshot.sequence)},
                         {"bid_levels", std::to_string(snapshot.bids.size())},
                         {"ask_levels", std::to_string(snapshot.asks.size())},
                         {"best_bid",
                          snapshot.bids.empty() ? "0" : snapshot.bids.front().price.to_string()},
                         {"best_ask",
                          snapshot.asks.empty() ? "0" : snapshot.asks.front().price.to_string()},
                         {"payload_bytes", std::to_string(payload.size())},
                         {"source_file", "cpp/venues/src/app/venues_loop.cpp"}});
}

void publish_raw_symbol_meta(cex::common::KafkaProducer* producer,
                             const domain::VenueRawSnapshot& snapshot) {
  fob::marketdata::v1::MarketDataRaw evt;
  auto* meta = evt.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("venues");
  meta->set_correlation_id(cex::common::uuid_v4());
  meta->set_partition_key(partition_key_for(snapshot));

  auto* symbol_meta = evt.mutable_symbol_meta();
  symbol_meta->set_venue(snapshot.venue_id);
  *symbol_meta->mutable_instrument() = snapshot.instrument;
  *symbol_meta->mutable_maker_fee_rate() = snapshot.fees.maker.to_proto();
  *symbol_meta->mutable_taker_fee_rate() = snapshot.fees.taker.to_proto();
  *symbol_meta->mutable_tick_size() = snapshot.trading_rules.tick_size.to_proto();
  *symbol_meta->mutable_lot_size() = snapshot.trading_rules.lot_size.to_proto();
  *symbol_meta->mutable_min_notional() = snapshot.trading_rules.min_notional.to_proto();
  *symbol_meta->mutable_min_qty() = snapshot.trading_rules.min_qty.to_proto();
  *symbol_meta->mutable_max_qty() = snapshot.trading_rules.max_qty.to_proto();
  symbol_meta->set_venue_symbol(snapshot.venue_symbol);

  const std::string payload = cex::common::to_bytes(evt);
  const bool ok = producer->produce("marketdata.raw", meta->partition_key(), payload);
  cex::common::log_json(ok ? "INFO" : "ERROR", "Published marketdata.raw symbol_meta",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "publish_raw_symbol_meta"},
                         {"topic", "marketdata.raw"},
                         {"event_id", meta->event_id()},
                         {"venue", snapshot.venue_id},
                         {"symbol", snapshot.instrument.symbol()},
                         {"venue_symbol", snapshot.venue_symbol},
                         {"maker_fee", snapshot.fees.maker.to_string()},
                         {"taker_fee", snapshot.fees.taker.to_string()},
                         {"tick_size", snapshot.trading_rules.tick_size.to_string()},
                         {"lot_size", snapshot.trading_rules.lot_size.to_string()},
                         {"min_notional", snapshot.trading_rules.min_notional.to_string()},
                         {"min_qty", snapshot.trading_rules.min_qty.to_string()},
                         {"max_qty", snapshot.trading_rules.max_qty.to_string()},
                         {"payload_bytes", std::to_string(payload.size())},
                         {"source_file", "cpp/venues/src/app/venues_loop.cpp"}});
}

void publish_raw_ticker(cex::common::KafkaProducer* producer,
                        const domain::VenueRawSnapshot& snapshot) {
  fob::marketdata::v1::MarketDataRaw evt;
  auto* meta = evt.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("venues");
  meta->set_correlation_id(cex::common::uuid_v4());
  meta->set_partition_key(partition_key_for(snapshot));

  auto* ticker = evt.mutable_ticker();
  ticker->set_venue(snapshot.venue_id);
  *ticker->mutable_instrument() = snapshot.instrument;
  *ticker->mutable_timestamp() = snapshot.timestamp;
  *ticker->mutable_bid() = snapshot.best_bid.to_proto();
  *ticker->mutable_ask() = snapshot.best_ask.to_proto();
  *ticker->mutable_last() = snapshot.mid_price.to_proto();
  *ticker->mutable_spread() = snapshot.spread.to_proto();
  *ticker->mutable_vwap() = snapshot.mid_price.to_proto();
  *ticker->mutable_base_volume() = snapshot.volume_24h.to_proto();
  *ticker->mutable_quote_volume() =
      Decimal::mul(snapshot.volume_24h, snapshot.mid_price).to_proto();

  const std::string payload = cex::common::to_bytes(evt);
  const bool ok = producer->produce("marketdata.raw", meta->partition_key(), payload);
  cex::common::log_json(ok ? "INFO" : "ERROR", "Published marketdata.raw ticker",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "publish_raw_ticker"},
                         {"topic", "marketdata.raw"},
                         {"event_id", meta->event_id()},
                         {"venue", snapshot.venue_id},
                         {"symbol", snapshot.instrument.symbol()},
                         {"venue_symbol", snapshot.venue_symbol},
                         {"bid", snapshot.best_bid.to_string()},
                         {"ask", snapshot.best_ask.to_string()},
                         {"mid", snapshot.mid_price.to_string()},
                         {"spread", snapshot.spread.to_string()},
                         {"base_volume_24h", snapshot.volume_24h.to_string()},
                         {"quote_volume_24h",
                          Decimal::mul(snapshot.volume_24h, snapshot.mid_price).to_string()},
                         {"payload_bytes", std::to_string(payload.size())},
                         {"source_file", "cpp/venues/src/app/venues_loop.cpp"}});
}

template <typename T>
void push_bounded(std::deque<T>* history, const T& value, const std::size_t capacity) {
  if (history == nullptr || capacity == 0) return;
  history->push_back(value);
  while (history->size() > capacity) {
    history->pop_front();
  }
}

}  // namespace

VenuesLoop::VenuesLoop(const std::string& brokers,
                       ISnapshotStorage* snapshot_storage)
    : brokers_(brokers),
      snapshot_storage_(snapshot_storage),
      producer_({.brokers=brokers, .client_id="venues"}),
      consumer_({.brokers=brokers,
                 .group_id=cex::common::Env::get_string(
                     "VENUES_EXEC_CONSUMER_GROUP", "venues_exec"),
                 .client_id="venues_exec",
                 .enable_auto_commit=false,
                 .auto_offset_reset=cex::common::Env::get_string(
                     "VENUES_EXEC_AUTO_OFFSET_RESET", "latest")}),
      observability_(cex::common::KafkaProducer(
          {.brokers=brokers, .client_id="venues_observability"})) {
  md_started_at_ = std::chrono::steady_clock::now();
  execution_report_producer_ = std::make_unique<infra::ExecutionReportProducer>(
      [this](const std::string& topic,
             const std::string& key,
             const std::string& payload) {
        return producer_.produce(topic, key, payload);
      });

  default_subscription_ = make_default_subscription();
  default_snapshot_request_ = make_default_snapshot_request();
  history_capacity_ = static_cast<std::size_t>(std::max(
      1, cex::common::Env::get_int("VENUES_API_HISTORY_CAPACITY", 200)));

  // F11-NORM-3/4/5: Create snapshot producer with status model + dedup + storage.
  kafka_publisher_ = std::make_unique<infra::KafkaMessagePublisher>(&producer_);
  SnapshotProducerConfig snap_cfg;
  snap_cfg.normalization.fee_scale = 6;
  snap_cfg.normalization.depth_config.max_levels_per_side = 0;
  snap_cfg.status.stale_threshold_ms = static_cast<int64_t>(
      cex::common::Env::get_int("STALE_THRESHOLD_MS", 15000));
  snap_cfg.topic = "venue.snapshots";
  snap_cfg.emit_publish_log = env_bool("VENUES_SNAPSHOT_LOG_LATENCY", false);
  snapshot_config_ = snap_cfg;
  snapshot_producer_ = std::make_unique<SnapshotProducer>(
      kafka_publisher_.get(), snapshot_storage, snapshot_config_);

  LiquidityCurveProducerConfig curve_cfg;
  curve_cfg.topic = cex::common::Env::get_string("VENUE_LIQUIDITY_TOPIC", "venue.liquidity.fob");
  curve_cfg.level = cex::common::Env::get_string(
      "CURVE_LEVEL",
      cex::common::Env::get_string("LOB_TO_FOB_DEFAULT_LEVEL", "L3"));
  const double tau_sec = std::max(
      0.001,
      env_double("LOB_TO_FOB_TAU_SEC", 5.0));
  curve_cfg.tau_ms = std::max(1.0, env_double("CURVE_TAU_MS", tau_sec * 1000.0));
  curve_cfg.liquidity_fob_versioning.schema_version = static_cast<uint32_t>(std::max(
      1, cex::common::Env::get_int(
             "VENUE_LIQUIDITY_SCHEMA_VERSION",
             static_cast<int>(curve_cfg.liquidity_fob_versioning.schema_version))));
  curve_cfg.liquidity_fob_versioning.min_compatible_schema_version =
      static_cast<uint32_t>(std::clamp(
          cex::common::Env::get_int(
              "VENUE_LIQUIDITY_MIN_COMPAT_SCHEMA_VERSION",
              static_cast<int>(
                  curve_cfg.liquidity_fob_versioning.min_compatible_schema_version)),
          1, static_cast<int>(curve_cfg.liquidity_fob_versioning.schema_version)));
  curve_cfg.liquidity_fob_versioning.producer_version =
      cex::common::Env::get_string(
          "VENUE_LIQUIDITY_PRODUCER_VERSION",
          curve_cfg.liquidity_fob_versioning.producer_version);
  curve_cfg.liquidity_fob_versioning.model_config_version =
      cex::common::Env::get_string(
          "CURVE_MODEL_CONFIG_VERSION",
          curve_cfg.liquidity_fob_versioning.model_config_version);
  curve_cfg.input.min_qty = env_decimal_units_scale(
      "CURVE_MIN_QTY_UNITS", "CURVE_MIN_QTY_SCALE", curve_cfg.input.min_qty);
  curve_cfg.input.apply_taker_fee = env_bool(
      "CURVE_APPLY_TAKER_FEE", curve_cfg.input.apply_taker_fee);
  curve_cfg.input.fee_adjusted_price_scale = std::clamp(
      cex::common::Env::get_int(
          "CURVE_FEE_PRICE_SCALE", curve_cfg.input.fee_adjusted_price_scale),
      0, 12);
  curve_cfg.apply_convexification = env_bool("CURVE_APPLY_CONVEXIFICATION", true);
  curve_cfg.apply_moreau_l2 = env_bool("CURVE_APPLY_MOREAU_L2", true);
  curve_cfg.apply_tikhonov_l2 = env_bool("CURVE_APPLY_TIKHONOV_L2", true);
  curve_cfg.apply_fenchel_legendre = env_bool("CURVE_APPLY_FENCHEL_LEGENDRE", true);
  curve_cfg.moreau_l2.smoothing =
      std::max(0.0, env_double("CURVE_MOREAU_MU", curve_cfg.moreau_l2.smoothing));
  curve_cfg.tikhonov_l2.curvature_penalty =
      std::max(0.0, env_double("CURVE_TIKHONOV_LAMBDA", curve_cfg.tikhonov_l2.curvature_penalty));
  curve_cfg.tikhonov_l2.max_iterations = static_cast<std::size_t>(std::max(
      1, cex::common::Env::get_int("CURVE_TIKHONOV_MAX_ITERS",
                                   static_cast<int>(curve_cfg.tikhonov_l2.max_iterations))));
  curve_cfg.tikhonov_l2.tolerance =
      std::max(0.0, env_double("CURVE_TIKHONOV_TOL", curve_cfg.tikhonov_l2.tolerance));
  curve_cfg.l3_impact.enabled = env_bool(
      "CURVE_L3_IMPACT_ENABLED", curve_cfg.level == "L3");
  curve_cfg.l3_impact.model = env_l3_impact_model();
  curve_cfg.l3_impact.max_history = static_cast<std::size_t>(std::max(
      1, cex::common::Env::get_int(
             "CURVE_L3_IMPACT_MAX_HISTORY",
             static_cast<int>(curve_cfg.l3_impact.max_history))));
  curve_cfg.l3_impact.max_relative_impact = std::max(
      0.0, env_double("CURVE_L3_IMPACT_MAX_REL", curve_cfg.l3_impact.max_relative_impact));
  curve_cfg.l3_impact.execution_blend_weight = std::clamp(
      env_double("CURVE_L3_EXEC_BLEND_WEIGHT", curve_cfg.l3_impact.execution_blend_weight),
      0.0, 1.0);
  curve_cfg.l3_impact.min_samples_for_full_weight = static_cast<std::size_t>(std::max(
      1, cex::common::Env::get_int(
             "CURVE_L3_MIN_SAMPLES_FOR_FULL_WEIGHT",
             static_cast<int>(curve_cfg.l3_impact.min_samples_for_full_weight))));
  curve_cfg.amm_direct.enabled = env_bool(
      "CURVE_AMM_DIRECT_ENABLED", curve_cfg.amm_direct.enabled);
  curve_cfg.amm_direct.compare_with_virtual_lob = env_bool(
      "CURVE_AMM_DIRECT_COMPARE_WITH_VLOB",
      curve_cfg.amm_direct.compare_with_virtual_lob);
  curve_cfg.amm_direct.max_segments_per_side = static_cast<std::size_t>(std::max(
      1, cex::common::Env::get_int(
             "CURVE_AMM_DIRECT_MAX_SEGMENTS_PER_SIDE",
             static_cast<int>(curve_cfg.amm_direct.max_segments_per_side))));
  curve_cfg.amm_direct.pool_fee_rate_override = env_double(
      "CURVE_AMM_DIRECT_POOL_FEE_RATE_OVERRIDE",
      curve_cfg.amm_direct.pool_fee_rate_override);
  curve_cfg.amm_direct.gas_cost_quote = std::max(
      0.0, env_double("CURVE_AMM_DIRECT_GAS_COST_QUOTE",
                      curve_cfg.amm_direct.gas_cost_quote));
  curve_cfg.amm_direct.execution_overhead_bps = std::max(
      0.0, env_double("CURVE_AMM_DIRECT_EXECUTION_OVERHEAD_BPS",
                      curve_cfg.amm_direct.execution_overhead_bps));
  curve_cfg.amm_direct.slippage_multiplier = std::max(
      1e-9, env_double("CURVE_AMM_DIRECT_SLIPPAGE_MULTIPLIER",
                       curve_cfg.amm_direct.slippage_multiplier));
  curve_cfg.enable_quality_gating = env_bool(
      "CURVE_QUALITY_GATING_ENABLED", curve_cfg.enable_quality_gating);
  curve_cfg.quality_thresholds.epsilon1_degrade = std::max(
      0.0, env_double("CURVE_EPSILON1_DEGRADE", curve_cfg.quality_thresholds.epsilon1_degrade));
  curve_cfg.quality_thresholds.epsilon1_disable = std::max(
      0.0, env_double("CURVE_EPSILON1_DISABLE", curve_cfg.quality_thresholds.epsilon1_disable));
  curve_cfg.quality_thresholds.epsilon2_degrade = std::max(
      0.0, env_double("CURVE_EPSILON2_DEGRADE", curve_cfg.quality_thresholds.epsilon2_degrade));
  curve_cfg.quality_thresholds.epsilon2_disable = std::max(
      0.0, env_double("CURVE_EPSILON2_DISABLE", curve_cfg.quality_thresholds.epsilon2_disable));
  curve_cfg.quality_thresholds.epsilon3_degrade = std::max(
      0.0, env_double("CURVE_EPSILON3_DEGRADE", curve_cfg.quality_thresholds.epsilon3_degrade));
  curve_cfg.quality_thresholds.epsilon3_disable = std::max(
      0.0, env_double("CURVE_EPSILON3_DISABLE", curve_cfg.quality_thresholds.epsilon3_disable));
  curve_cfg.degradation.enabled = env_bool(
      "CURVE_DEGRADATION_ENABLED", curve_cfg.degradation.enabled);
  curve_cfg.degradation.epsilon1_green_bps = std::max(
      1e-9, env_double("CURVE_EPSILON1_GREEN_BPS",
                       curve_cfg.degradation.epsilon1_green_bps));
  curve_cfg.degradation.epsilon2_green_bps = std::max(
      1e-9, env_double("CURVE_EPSILON2_GREEN_BPS",
                       curve_cfg.degradation.epsilon2_green_bps));
  curve_cfg.degradation.epsilon3_green_bps = std::max(
      1e-9, env_double("CURVE_EPSILON3_GREEN_BPS",
                       curve_cfg.degradation.epsilon3_green_bps));
  curve_cfg.degradation.min_l3_confidence = std::clamp(
      env_double("CURVE_MIN_L3_CONFIDENCE", curve_cfg.degradation.min_l3_confidence),
      0.0, 1.0);
  curve_cfg.degradation.min_l2_confidence = std::clamp(
      env_double("CURVE_MIN_L2_CONFIDENCE", curve_cfg.degradation.min_l2_confidence),
      0.0, 1.0);
  curve_cfg.degradation.min_l1_confidence = std::clamp(
      env_double("CURVE_MIN_L1_CONFIDENCE", curve_cfg.degradation.min_l1_confidence),
      0.0, 1.0);
  curve_cfg.degradation.stale_confidence_multiplier = std::clamp(
      env_double("CURVE_STALE_CONFIDENCE_MULTIPLIER",
                 curve_cfg.degradation.stale_confidence_multiplier),
      0.0, 1.0);
  curve_cfg.degradation.l3_no_execution_confidence_multiplier = std::clamp(
      env_double("CURVE_L3_NO_EXEC_CONFIDENCE_MULTIPLIER",
                 curve_cfg.degradation.l3_no_execution_confidence_multiplier),
      0.0, 1.0);
  curve_cfg.degradation.error_confidence_penalty = std::clamp(
      env_double("CURVE_ERROR_CONFIDENCE_PENALTY",
                 curve_cfg.degradation.error_confidence_penalty),
      0.0, 1.0);
  curve_cfg.degradation.max_consecutive_errors_off = static_cast<uint32_t>(std::max(
      1, cex::common::Env::get_int(
             "CURVE_MAX_CONSECUTIVE_ERRORS_OFF",
             static_cast<int>(curve_cfg.degradation.max_consecutive_errors_off))));
  curve_cfg.degradation.publish_stale_l1_fallback = env_bool(
      "CURVE_PUBLISH_STALE_L1_FALLBACK",
      curve_cfg.degradation.publish_stale_l1_fallback);

  curve_cfg.synthetic.enabled = env_bool(
      "CURVE_SYNTHETIC_ENABLED", curve_cfg.synthetic.enabled);
  curve_cfg.synthetic.topic = cex::common::Env::get_string(
      "VENUE_SYNTHETIC_TOPIC", curve_cfg.synthetic.topic);
  curve_cfg.synthetic.liquidity_source = cex::common::Env::get_string(
      "VENUE_SYNTHETIC_LIQUIDITY_SOURCE", curve_cfg.synthetic.liquidity_source);
  curve_cfg.synthetic.ttl_ms = static_cast<int64_t>(std::max(
      1, cex::common::Env::get_int(
             "VENUE_SYNTHETIC_TTL_MS",
             static_cast<int>(curve_cfg.synthetic.ttl_ms))));
  curve_cfg.synthetic.price_scale = cex::common::Env::get_int(
      "VENUE_SYNTHETIC_PRICE_SCALE", curve_cfg.synthetic.price_scale);
  curve_cfg.synthetic.quantity_scale = cex::common::Env::get_int(
      "VENUE_SYNTHETIC_QTY_SCALE", curve_cfg.synthetic.quantity_scale);
  curve_config_ = curve_cfg;

  ISyntheticOrderRepository* synthetic_order_repository = nullptr;
  const auto venues_postgres_dsn = cex::common::Env::try_get_string("VENUES_POSTGRES_DSN");
  if (curve_config_.synthetic.enabled &&
      venues_postgres_dsn.has_value() &&
      !venues_postgres_dsn->empty()) {
    synthetic_order_repository_ =
        std::make_unique<infra::PostgresSyntheticOrderRepository>(*venues_postgres_dsn);
    synthetic_order_repository = synthetic_order_repository_.get();
    cex::common::log_json("INFO", "Venue synthetic_orders Postgres repository enabled",
                          {{"env", "VENUES_POSTGRES_DSN"}});
  } else if (curve_config_.synthetic.enabled) {
    cex::common::log_json("WARN", "Venue synthetic_orders Postgres repository disabled",
                          {{"reason", "VENUES_POSTGRES_DSN not set"}});
  }

  liquidity_curve_producer_ = std::make_unique<LiquidityCurveProducer>(
      kafka_publisher_.get(), synthetic_order_repository, curve_config_);

  std::string mode = cex::common::Env::get_string("VENUES_ADAPTER_MODE", "simulated");

  // F12-BACKTEST-1: when backtest mode is requested, replace the External
  // Venues Connector adapters with VenueSim. The Venue Execution Adapter
  // (ExecuteOnVenue), Execution Planning, Risk Manager and Settlement
  // Ledger keep their business logic intact — only the wire endpoint of
  // PlaceChildOrder is swapped.
  const bool backtest_mode =
      env_bool("VENUES_BACKTEST_MODE", false) || mode == "backtest_venuesim";
  if (backtest_mode) {
    mode = "backtest_venuesim";
    const std::string session_id =
        cex::common::Env::get_string("VENUES_BACKTEST_SESSION_ID", "");
    const std::string ids_text = cex::common::Env::get_string(
        "VENUES_BACKTEST_VENUE_IDS", "binance,coinbase,uniswap_v3");

    std::vector<std::string> venue_ids;
    std::string cur;
    for (const char c : ids_text) {
      if (c == ',' || c == ';' || c == ' ') {
        if (!cur.empty()) {
          venue_ids.push_back(cur);
          cur.clear();
        }
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) venue_ids.push_back(cur);
    if (venue_ids.empty()) venue_ids.push_back("binance");

    auto venue_type_for = [](const std::string& venue_id) {
      std::string lower = venue_id;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (lower.find("uniswap") != std::string::npos ||
          lower.find("curve") != std::string::npos ||
          lower.find("dex") != std::string::npos) {
        return domain::VenueType::kDex;
      }
      if (lower.find("amm") != std::string::npos) return domain::VenueType::kAmm;
      return domain::VenueType::kCex;
    };

    for (const auto& venue_id : venue_ids) {
      adapters_.push_back(std::make_unique<infra::VenueSimAdapter>(
          venue_id, venue_type_for(venue_id), session_id));
    }

    cex::common::log_json(
        "INFO",
        "VenuesLoop selected VenueSim backtest adapters (F12-BACKTEST-1)",
        {{"mode", mode},
         {"session_id", session_id},
         {"venue_ids", ids_text}});

    // F12-BACKTEST-5: stamp ExecutionReports with the backtest session
    // and route them to the backtest.execution.venue topic so replay
    // history never mixes with production execution.venue history.
    if (!session_id.empty() && execution_report_producer_ != nullptr) {
      infra::BacktestSession bt_session;
      bt_session.session_id = session_id;
      bt_session.namespace_id = cex::common::Env::get_string(
          "VENUES_BACKTEST_NAMESPACE_ID", "");
      execution_report_producer_->SetBacktestSession(bt_session);
      cex::common::log_json(
          "INFO",
          "VenuesLoop scoped ExecutionReportProducer to backtest session "
          "(F12-BACKTEST-5)",
          {{"session_id", session_id},
           {"namespace_id", bt_session.ResolveNamespace()},
           {"topic", "backtest.execution.venue"}});
    }
  } else if (mode == "real_multi_2cex_1dex" || mode == "multi_real") {
    auto apply_real_cex_defaults = [](infra::CexWsRestAdapterConfig* cfg) {
      if (cfg == nullptr) return;
      cfg->reconnect_cooldown_ms = static_cast<uint32_t>(
          std::max(1000, cex::common::Env::get_int("CEX_RECONNECT_COOLDOWN_MS", 300000)));
      cfg->heartbeat_ping_interval_ms = static_cast<uint32_t>(
          std::max(1000, cex::common::Env::get_int("CEX_HEARTBEAT_PING_INTERVAL_MS", 15000)));
      cfg->heartbeat_pong_timeout_ms = static_cast<uint32_t>(
          std::max(1000, cex::common::Env::get_int("CEX_HEARTBEAT_PONG_TIMEOUT_MS", 15000)));
      cfg->stale_threshold_ms = static_cast<uint32_t>(
          std::max(1000, cex::common::Env::get_int("STALE_THRESHOLD_MS", 15000)));
      cfg->rest_timeout_ms = static_cast<uint32_t>(
          std::max(1000, cex::common::Env::get_int("CEX_REST_TIMEOUT_MS", 5000)));
      cfg->rest_requests_per_sec = env_double("CEX_REST_RPS", 2.0);
      cfg->rest_burst = env_double("CEX_REST_BURST", 4.0);
      cfg->rest_ticker_volume_enabled = env_bool("CEX_REST_TICKER_VOLUME_ENABLED", true);
      cfg->simulate_orders = env_bool(
          "CEX_SIMULATE_ORDERS",
          env_bool("VENUES_SIMULATE_ORDERS", true));
      cfg->circuit_breaker_enabled = env_bool(
          "CIRCUIT_BREAKER_ENABLED", cfg->circuit_breaker_enabled);
      cfg->circuit_breaker_errors = static_cast<uint32_t>(
          std::max(1, cex::common::Env::get_int(
                          "CIRCUIT_BREAKER_ERRORS",
                          static_cast<int>(cfg->circuit_breaker_errors))));
      cfg->circuit_breaker_window_ms = static_cast<uint32_t>(
          std::max(1, cex::common::Env::get_int(
                          "CIRCUIT_BREAKER_WINDOW_S",
                          static_cast<int>(cfg->circuit_breaker_window_ms / 1000)))) *
          1000U;
      cfg->circuit_breaker_cooldown_ms = static_cast<uint32_t>(
          std::max(1, cex::common::Env::get_int(
                          "CIRCUIT_BREAKER_COOLDOWN_S",
                          static_cast<int>(cfg->circuit_breaker_cooldown_ms / 1000)))) *
          1000U;
    };

    infra::CexWsRestAdapterConfig binance_cfg;
    binance_cfg.venue_id = cex::common::Env::get_string("BINANCE_VENUE_ID", "binance");
    binance_cfg.ws_url = cex::common::Env::get_string(
        "BINANCE_WS_URL", "wss://stream.binance.com:9443/ws");
    binance_cfg.rest_base_url = cex::common::Env::get_string(
        "BINANCE_REST_BASE_URL", "https://api.binance.com");
    apply_real_cex_defaults(&binance_cfg);
    adapters_.push_back(std::make_unique<infra::CexWsRestAdapter>(binance_cfg));

    infra::CexWsRestAdapterConfig coinbase_cfg;
    coinbase_cfg.venue_id = cex::common::Env::get_string("COINBASE_VENUE_ID", "coinbase");
    coinbase_cfg.ws_url = cex::common::Env::get_string(
        "COINBASE_WS_URL", "wss://ws-feed.exchange.coinbase.com");
    coinbase_cfg.rest_base_url = cex::common::Env::get_string(
        "COINBASE_REST_BASE_URL", "https://api.exchange.coinbase.com");
    apply_real_cex_defaults(&coinbase_cfg);
    adapters_.push_back(std::make_unique<infra::CexWsRestAdapter>(coinbase_cfg));

    infra::DexAmmRpcAdapterConfig uniswap_cfg;
    uniswap_cfg.venue_id = cex::common::Env::get_string("UNISWAP_VENUE_ID", "uniswap_v3");
    uniswap_cfg.rpc_url = cex::common::Env::get_string(
        "UNISWAP_API_URL", "https://api.dexscreener.com");
    uniswap_cfg.chain_id = cex::common::Env::get_string("UNISWAP_CHAIN_ID", "ethereum");
    uniswap_cfg.pool_address = cex::common::Env::get_string(
        "UNISWAP_POOL_ADDRESS", "0xcbcdf9626bc03e24f779434178a73a0b4bad62ed");
    uniswap_cfg.sync_mode = infra::DexSyncMode::kPolling;
    uniswap_cfg.finalization_class = infra::DexFinalizationClass::kFast;
    uniswap_cfg.polling_interval_fast_ms = static_cast<uint32_t>(
        std::max(1000, cex::common::Env::get_int("DEX_POLLING_INTERVAL_FAST_MS", 5000)));
    uniswap_cfg.polling_interval_slow_ms = static_cast<uint32_t>(
        std::max(5000, cex::common::Env::get_int("DEX_POLLING_INTERVAL_SLOW_MS", 15000)));
    uniswap_cfg.stale_threshold_ms = static_cast<uint32_t>(
        std::max(1000, cex::common::Env::get_int("STALE_THRESHOLD_MS", 15000)));
    uniswap_cfg.reconnect_cooldown_ms = static_cast<uint32_t>(
        std::max(1000, cex::common::Env::get_int("DEX_RECONNECT_COOLDOWN_MS", 300000)));
    uniswap_cfg.heartbeat_pong_timeout_ms = static_cast<uint32_t>(
        std::max(1000, cex::common::Env::get_int("DEX_HEARTBEAT_PONG_TIMEOUT_MS", 15000)));
    uniswap_cfg.rpc_timeout_ms = static_cast<uint32_t>(
        std::max(1000, cex::common::Env::get_int("DEX_RPC_TIMEOUT_MS", 5000)));
    uniswap_cfg.simulate_orders = env_bool(
        "DEX_SIMULATE_ORDERS",
        env_bool("VENUES_SIMULATE_ORDERS", true));
    uniswap_cfg.rpc_requests_per_sec = env_double("DEX_RPC_RPS", 2.0);
    uniswap_cfg.rpc_burst = env_double("DEX_RPC_BURST", 4.0);
    uniswap_cfg.circuit_breaker_enabled = env_bool(
        "CIRCUIT_BREAKER_ENABLED", uniswap_cfg.circuit_breaker_enabled);
    uniswap_cfg.circuit_breaker_errors = static_cast<uint32_t>(
        std::max(1, cex::common::Env::get_int(
                        "CIRCUIT_BREAKER_ERRORS",
                        static_cast<int>(uniswap_cfg.circuit_breaker_errors))));
    uniswap_cfg.circuit_breaker_window_ms = static_cast<uint32_t>(
        std::max(1, cex::common::Env::get_int(
                        "CIRCUIT_BREAKER_WINDOW_S",
                        static_cast<int>(uniswap_cfg.circuit_breaker_window_ms / 1000)))) *
        1000U;
    uniswap_cfg.circuit_breaker_cooldown_ms = static_cast<uint32_t>(
        std::max(1, cex::common::Env::get_int(
                        "CIRCUIT_BREAKER_COOLDOWN_S",
                        static_cast<int>(uniswap_cfg.circuit_breaker_cooldown_ms / 1000)))) *
        1000U;
    adapters_.push_back(std::make_unique<infra::DexAmmRpcAdapter>(uniswap_cfg));

    cex::common::log_json(
        "INFO",
        "VenuesLoop selected real multi-venue adapters",
        {{"mode", mode},
         {"cex_1", binance_cfg.venue_id},
         {"cex_2", coinbase_cfg.venue_id},
         {"dex_1", uniswap_cfg.venue_id}});
  } else if (mode == "cex_ws_rest") {
    infra::CexWsRestAdapterConfig cfg;
    cfg.venue_id = cex::common::Env::get_string("CEX_VENUE_ID", "binance");
    cfg.ws_url = cex::common::Env::get_string("CEX_WS_URL", "");
    cfg.rest_base_url = cex::common::Env::get_string("CEX_REST_BASE_URL", "");
    cfg.api_key = cex::common::Env::get_string("CEX_API_KEY", "");
    cfg.api_secret = cex::common::Env::get_string("CEX_API_SECRET", "");
    cfg.reconnect_max_attempts = static_cast<uint32_t>(
        cex::common::Env::get_int("CEX_RECONNECT_MAX_ATTEMPTS", 5));
    cfg.reconnect_delay_ms = static_cast<uint32_t>(
        cex::common::Env::get_int("CEX_RECONNECT_DELAY_MS", 1000));
    cfg.reconnect_cooldown_ms = static_cast<uint32_t>(
        cex::common::Env::get_int("CEX_RECONNECT_COOLDOWN_MS", 300000));
    cfg.heartbeat_ping_interval_ms = static_cast<uint32_t>(
        cex::common::Env::get_int("CEX_HEARTBEAT_PING_INTERVAL_MS", 15000));
    cfg.heartbeat_pong_timeout_ms = static_cast<uint32_t>(
        cex::common::Env::get_int("CEX_HEARTBEAT_PONG_TIMEOUT_MS", 15000));
    cfg.stale_threshold_ms = static_cast<uint32_t>(
        cex::common::Env::get_int("STALE_THRESHOLD_MS", 15000));
    cfg.rest_timeout_ms = static_cast<uint32_t>(
        cex::common::Env::get_int("CEX_REST_TIMEOUT_MS", 5000));
    cfg.rest_requests_per_sec = env_double("CEX_REST_RPS", 10.0);
    cfg.rest_burst = env_double("CEX_REST_BURST", 10.0);
    cfg.ws_messages_per_sec = env_double("CEX_WS_RPS", 10.0);
    cfg.ws_burst = env_double("CEX_WS_BURST", 10.0);
    cfg.rest_ticker_volume_enabled = env_bool("CEX_REST_TICKER_VOLUME_ENABLED", true);
    cfg.simulate_orders = env_bool(
        "CEX_SIMULATE_ORDERS",
        env_bool("VENUES_SIMULATE_ORDERS", true));
    cfg.circuit_breaker_enabled = env_bool(
        "CIRCUIT_BREAKER_ENABLED", cfg.circuit_breaker_enabled);
    cfg.circuit_breaker_errors = static_cast<uint32_t>(
        std::max(1, cex::common::Env::get_int(
                        "CIRCUIT_BREAKER_ERRORS",
                        static_cast<int>(cfg.circuit_breaker_errors))));
    cfg.circuit_breaker_window_ms = static_cast<uint32_t>(
        std::max(1, cex::common::Env::get_int(
                        "CIRCUIT_BREAKER_WINDOW_S",
                        static_cast<int>(cfg.circuit_breaker_window_ms / 1000)))) *
        1000U;
    cfg.circuit_breaker_cooldown_ms = static_cast<uint32_t>(
        std::max(1, cex::common::Env::get_int(
                        "CIRCUIT_BREAKER_COOLDOWN_S",
                        static_cast<int>(cfg.circuit_breaker_cooldown_ms / 1000)))) *
        1000U;

    adapters_.push_back(std::make_unique<infra::CexWsRestAdapter>(cfg));
    cex::common::log_json("INFO", "VenuesLoop selected CEX WS/REST adapter",
                          {{"mode", mode}, {"venue", cfg.venue_id}});
  } else if (mode == "simulated_multi" || mode == "multi_2cex_1dex") {
    const std::string cex_primary_venue =
        cex::common::Env::get_string("SIM_MULTI_CEX_PRIMARY_VENUE_ID", "binance");
    const std::string cex_secondary_venue =
        cex::common::Env::get_string("SIM_MULTI_CEX_SECONDARY_VENUE_ID", "coinbase");
    const std::string dex_venue =
        cex::common::Env::get_string("SIM_MULTI_DEX_VENUE_ID", "uniswap_v3");
    adapters_.push_back(std::make_unique<infra::SimulatedVenueAdapter>(
        cex_primary_venue, domain::VenueType::kCex));
    adapters_.push_back(std::make_unique<infra::SimulatedVenueAdapter>(
        cex_secondary_venue, domain::VenueType::kCex));
    adapters_.push_back(std::make_unique<infra::SimulatedVenueAdapter>(
        dex_venue, domain::VenueType::kDex));
    cex::common::log_json(
        "INFO",
        "VenuesLoop selected simulated multi-venue adapters",
        {{"mode", mode},
         {"cex_primary", cex_primary_venue},
         {"cex_secondary", cex_secondary_venue},
         {"dex", dex_venue}});
  } else if (mode == "dex_amm_rpc") {
    infra::DexAmmRpcAdapterConfig cfg;
    cfg.venue_id = cex::common::Env::get_string("DEX_VENUE_ID", "uniswap_v3");
    cfg.rpc_url = cex::common::Env::get_string("DEX_RPC_URL", "");
    cfg.ws_url = cex::common::Env::get_string("DEX_WS_URL", "");
    cfg.chain_id = cex::common::Env::get_string("DEX_CHAIN_ID", "eth-mainnet");
    cfg.pool_address = cex::common::Env::get_string("DEX_POOL_ADDRESS", "");

    cfg.sync_mode = env_dex_sync_mode();
    cfg.finalization_class = env_dex_finalization_class();

    cfg.polling_interval_fast_ms = static_cast<uint32_t>(
        std::max(1000, cex::common::Env::get_int("DEX_POLLING_INTERVAL_FAST_MS", 5000)));
    cfg.polling_interval_slow_ms = static_cast<uint32_t>(
        std::max(5000, cex::common::Env::get_int("DEX_POLLING_INTERVAL_SLOW_MS", 15000)));
    cfg.stale_threshold_ms = static_cast<uint32_t>(
        std::max(1000, cex::common::Env::get_int("STALE_THRESHOLD_MS", 15000)));

    cfg.reconnect_max_attempts = static_cast<uint32_t>(
        cex::common::Env::get_int("DEX_RECONNECT_MAX_ATTEMPTS", 5));
    cfg.reconnect_delay_ms = static_cast<uint32_t>(
        cex::common::Env::get_int("DEX_RECONNECT_DELAY_MS", 1500));
    cfg.reconnect_cooldown_ms = static_cast<uint32_t>(
        cex::common::Env::get_int("DEX_RECONNECT_COOLDOWN_MS", 300000));
    cfg.heartbeat_ping_interval_ms = static_cast<uint32_t>(
        cex::common::Env::get_int("DEX_HEARTBEAT_PING_INTERVAL_MS", 15000));
    cfg.heartbeat_pong_timeout_ms = static_cast<uint32_t>(
        cex::common::Env::get_int("DEX_HEARTBEAT_PONG_TIMEOUT_MS", 15000));
    cfg.rpc_timeout_ms = static_cast<uint32_t>(
        cex::common::Env::get_int("DEX_RPC_TIMEOUT_MS", 5000));

    cfg.rpc_requests_per_sec = env_double("DEX_RPC_RPS", 12.0);
    cfg.rpc_burst = env_double("DEX_RPC_BURST", 12.0);
    cfg.ws_messages_per_sec = env_double("DEX_WS_RPS", 10.0);
    cfg.ws_burst = env_double("DEX_WS_BURST", 10.0);
    cfg.simulate_orders = env_bool(
        "DEX_SIMULATE_ORDERS",
        env_bool("VENUES_SIMULATE_ORDERS", true));
    cfg.circuit_breaker_enabled = env_bool(
        "CIRCUIT_BREAKER_ENABLED", cfg.circuit_breaker_enabled);
    cfg.circuit_breaker_errors = static_cast<uint32_t>(
        std::max(1, cex::common::Env::get_int(
                        "CIRCUIT_BREAKER_ERRORS",
                        static_cast<int>(cfg.circuit_breaker_errors))));
    cfg.circuit_breaker_window_ms = static_cast<uint32_t>(
        std::max(1, cex::common::Env::get_int(
                        "CIRCUIT_BREAKER_WINDOW_S",
                        static_cast<int>(cfg.circuit_breaker_window_ms / 1000)))) *
        1000U;
    cfg.circuit_breaker_cooldown_ms = static_cast<uint32_t>(
        std::max(1, cex::common::Env::get_int(
                        "CIRCUIT_BREAKER_COOLDOWN_S",
                        static_cast<int>(cfg.circuit_breaker_cooldown_ms / 1000)))) *
        1000U;

    cfg.max_swap_events = static_cast<std::size_t>(
        std::max(0, cex::common::Env::get_int("DEX_MAX_SWAP_EVENTS", 128)));
    cfg.default_depth_levels = static_cast<std::size_t>(
        std::max(1, cex::common::Env::get_int("DEX_DEFAULT_DEPTH_LEVELS", 16)));
    cfg.market_price_scale = cex::common::Env::get_int("DEX_MARKET_PRICE_SCALE", 8);
    cfg.market_qty_scale = cex::common::Env::get_int("DEX_MARKET_QTY_SCALE", 8);

    adapters_.push_back(std::make_unique<infra::DexAmmRpcAdapter>(cfg));
    cex::common::log_json("INFO", "VenuesLoop selected DEX/AMM RPC adapter",
                          {{"mode", mode}, {"venue", cfg.venue_id}});
  } else {
    const std::string simulated_venue_id =
        cex::common::Env::get_string("SIMULATED_VENUE_ID", "binance");
    adapters_.push_back(std::make_unique<infra::SimulatedVenueAdapter>(simulated_venue_id));
    cex::common::log_json("INFO", "VenuesLoop selected simulated adapter",
                          {{"mode", mode}, {"venue", simulated_venue_id}});
  }

  if (!adapters_.empty()) {
    const std::string cex_ws_url = cex::common::Env::get_string("CEX_WS_URL", "");
    const std::string cex_rest_base_url = cex::common::Env::get_string("CEX_REST_BASE_URL", "");
    const std::string binance_ws_url = cex::common::Env::get_string(
        "BINANCE_WS_URL", "wss://stream.binance.com:9443/ws");
    const std::string binance_rest_base_url = cex::common::Env::get_string(
        "BINANCE_REST_BASE_URL", "https://api.binance.com");
    const std::string coinbase_ws_url = cex::common::Env::get_string(
        "COINBASE_WS_URL", "wss://ws-feed.exchange.coinbase.com");
    const std::string coinbase_rest_base_url = cex::common::Env::get_string(
        "COINBASE_REST_BASE_URL", "https://api.exchange.coinbase.com");
    const std::string dex_ws_url = cex::common::Env::get_string("DEX_WS_URL", "");
    const std::string dex_rpc_url = cex::common::Env::get_string("DEX_RPC_URL", "");
    const std::string uniswap_api_url = cex::common::Env::get_string(
        "UNISWAP_API_URL", "https://api.dexscreener.com");
    const std::string dex_chain_id = cex::common::Env::get_string("DEX_CHAIN_ID", "");
    const std::string dex_pool_address = cex::common::Env::get_string("DEX_POOL_ADDRESS", "");
    const std::string uniswap_chain_id = cex::common::Env::get_string("UNISWAP_CHAIN_ID", "ethereum");
    const std::string uniswap_pool_address = cex::common::Env::get_string(
        "UNISWAP_POOL_ADDRESS", "0xcbcdf9626bc03e24f779434178a73a0b4bad62ed");

    for (const auto& adapter : adapters_) {
      VenueConfigRecord config;
      config.venue_id = adapter->VenueId();
      config.adapter_mode = mode;
      if ((mode == "real_multi_2cex_1dex" || mode == "multi_real") &&
          adapter->VenueId().find("coinbase") != std::string::npos) {
        config.ws_url = coinbase_ws_url;
        config.rest_base_url = coinbase_rest_base_url;
      } else if ((mode == "real_multi_2cex_1dex" || mode == "multi_real") &&
                 adapter->Type() == domain::VenueType::kCex) {
        config.ws_url = binance_ws_url;
        config.rest_base_url = binance_rest_base_url;
      } else if (adapter->Type() == domain::VenueType::kDex ||
                 adapter->Type() == domain::VenueType::kAmm) {
        config.ws_url = dex_ws_url;
        config.rpc_url = (mode == "real_multi_2cex_1dex" || mode == "multi_real")
            ? uniswap_api_url
            : dex_rpc_url;
        config.chain_id = (mode == "real_multi_2cex_1dex" || mode == "multi_real")
            ? uniswap_chain_id
            : dex_chain_id;
        config.pool_address = (mode == "real_multi_2cex_1dex" || mode == "multi_real")
            ? uniswap_pool_address
            : dex_pool_address;
      } else {
        config.ws_url = cex_ws_url;
        config.rest_base_url = cex_rest_base_url;
      }
      if (mode == "real_multi_2cex_1dex" || mode == "multi_real" ||
          mode == "simulated_multi" || mode == "multi_2cex_1dex") {
        if (adapter->Type() == domain::VenueType::kDex ||
            adapter->Type() == domain::VenueType::kAmm) {
          config.venue_symbol = cex::common::Env::get_string(
              "UNISWAP_VENUE_SYMBOL", "WBTCUSDC");
        } else if (adapter->VenueId().find("coinbase") != std::string::npos) {
          config.venue_symbol = cex::common::Env::get_string(
              "COINBASE_VENUE_SYMBOL", "BTC-USD");
        } else {
          config.venue_symbol = cex::common::Env::get_string(
              "BINANCE_VENUE_SYMBOL", "BTCUSDT");
        }
      } else {
        config.venue_symbol = default_snapshot_request_.venue_symbol;
      }
      config.depth_levels = static_cast<uint32_t>(default_snapshot_request_.depth_levels);
      config.curve_level = curve_config_.level;
      config.synthetic_enabled = curve_config_.synthetic.enabled;
      config.stale_threshold_ms =
          static_cast<uint32_t>(std::max<int64_t>(1, snapshot_config_.status.stale_threshold_ms));
      config.circuit_breaker_enabled = env_bool("CIRCUIT_BREAKER_ENABLED", true);
      config.circuit_breaker_errors = static_cast<uint32_t>(
          std::max(1, cex::common::Env::get_int("CIRCUIT_BREAKER_ERRORS", 3)));
      config.circuit_breaker_window_ms = static_cast<uint32_t>(
          std::max(1, cex::common::Env::get_int("CIRCUIT_BREAKER_WINDOW_S", 60))) * 1000U;
      config.circuit_breaker_cooldown_ms = static_cast<uint32_t>(
          std::max(1, cex::common::Env::get_int("CIRCUIT_BREAKER_COOLDOWN_S", 300))) * 1000U;
      config.is_active = true;
      config.routing_mode = "auto";
      config.updated_at_ms = now_epoch_ms();
      venue_configs_[config.venue_id] = config;
      execute_on_venue_.SetVenueSymbolOverride(config.venue_id, config.venue_symbol);
    }
  }
}

std::vector<VenueConfigRecord> VenuesLoop::ListVenueConfigs() const {
  std::lock_guard<std::mutex> lock(config_mu_);
  std::vector<VenueConfigRecord> out;
  out.reserve(venue_configs_.size());
  for (const auto& [venue_id, cfg] : venue_configs_) {
    (void)venue_id;
    out.push_back(cfg);
  }
  std::sort(out.begin(), out.end(), [](const VenueConfigRecord& lhs, const VenueConfigRecord& rhs) {
    return lhs.venue_id < rhs.venue_id;
  });
  return out;
}

std::optional<VenueConfigRecord> VenuesLoop::GetVenueConfig(const std::string& venue_id) const {
  std::lock_guard<std::mutex> lock(config_mu_);
  const auto it = venue_configs_.find(venue_id);
  if (it == venue_configs_.end()) return std::nullopt;
  return it->second;
}

void VenuesLoop::reload_producers_locked() {
  ISyntheticOrderRepository* synthetic_repo = synthetic_order_repository_.get();
  snapshot_producer_ = std::make_unique<SnapshotProducer>(
      kafka_publisher_.get(), snapshot_storage_, snapshot_config_);
  liquidity_curve_producer_ = std::make_unique<LiquidityCurveProducer>(
      kafka_publisher_.get(), synthetic_repo, curve_config_);
}

void VenuesLoop::apply_runtime_config_locked(const VenueConfigRecord& config) {
  execute_on_venue_.SetVenueSymbolOverride(config.venue_id, config.venue_symbol);

  domain::VenueAdapterRuntimeConfig runtime_cfg;
  if (!config.ws_url.empty()) runtime_cfg.ws_url = config.ws_url;
  if (!config.rest_base_url.empty()) runtime_cfg.rest_base_url = config.rest_base_url;
  if (!config.rpc_url.empty()) runtime_cfg.rpc_url = config.rpc_url;
  if (!config.chain_id.empty()) runtime_cfg.chain_id = config.chain_id;
  if (!config.pool_address.empty()) runtime_cfg.pool_address = config.pool_address;
  runtime_cfg.stale_threshold_ms = std::max<uint32_t>(1, config.stale_threshold_ms);
  runtime_cfg.circuit_breaker_enabled = config.circuit_breaker_enabled;
  runtime_cfg.circuit_breaker_errors = std::max<uint32_t>(1, config.circuit_breaker_errors);
  runtime_cfg.circuit_breaker_window_ms = std::max<uint32_t>(1, config.circuit_breaker_window_ms);
  runtime_cfg.circuit_breaker_cooldown_ms = std::max<uint32_t>(1, config.circuit_breaker_cooldown_ms);

  domain::VenueSubscription subscription = default_subscription_;
  if (!config.venue_symbol.empty()) {
    subscription.venue_symbol = config.venue_symbol;
  }
  subscription.depth_levels =
      std::max<std::size_t>(1, static_cast<std::size_t>(config.depth_levels));

  for (auto& adapter : adapters_) {
    if (adapter->VenueId() == config.venue_id) {
      (void)adapter->ApplyRuntimeConfig(runtime_cfg);
      if (!adapter->Subscribe({subscription})) {
        (void)adapter->Reconnect();
        (void)adapter->Subscribe({subscription});
      }
    }
  }
}

bool VenuesLoop::UpsertVenueConfig(const VenueConfigRecord& in_config, std::string* error) {
  if (in_config.venue_id.empty()) {
    if (error != nullptr) *error = "venue_id must not be empty";
    return false;
  }

  VenueConfigRecord config = in_config;
  config.depth_levels = std::max<uint32_t>(1, config.depth_levels);
  config.stale_threshold_ms = std::max<uint32_t>(1, config.stale_threshold_ms);
  config.circuit_breaker_errors = std::max<uint32_t>(1, config.circuit_breaker_errors);
  config.circuit_breaker_window_ms = std::max<uint32_t>(1, config.circuit_breaker_window_ms);
  config.circuit_breaker_cooldown_ms = std::max<uint32_t>(1, config.circuit_breaker_cooldown_ms);
  config.curve_level = config.curve_level.empty() ? "L3" : config.curve_level;
  config.routing_mode = normalize_routing_mode(config.routing_mode);
  config.updated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  {
    std::lock_guard<std::mutex> lock(runtime_mu_);
    apply_runtime_config_locked(config);
  }
  {
    std::lock_guard<std::mutex> lock(config_mu_);
    venue_configs_[config.venue_id] = config;
  }
  return true;
}

bool VenuesLoop::DeleteVenueConfig(const std::string& venue_id) {
  VenueConfigRecord config;
  {
    std::lock_guard<std::mutex> lock(config_mu_);
    const auto it = venue_configs_.find(venue_id);
    if (it == venue_configs_.end()) return false;
    config = it->second;
    config.is_active = false;
    config.updated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    it->second = config;
  }
  {
    std::lock_guard<std::mutex> lock(runtime_mu_);
    apply_runtime_config_locked(config);
  }
  return true;
}

bool VenuesLoop::ForceReconnect(const std::string& venue_id, std::string* error) {
  if (venue_id.empty()) {
    if (error != nullptr) *error = "venue_id must not be empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(runtime_mu_);
  for (auto& adapter : adapters_) {
    if (adapter->VenueId() != venue_id) continue;
    const bool ok = adapter->Reconnect();
    if (!ok && error != nullptr) {
      *error = "adapter reconnect failed";
    }
    return ok;
  }

  if (error != nullptr) *error = "venue adapter not found";
  return false;
}

std::vector<domain::VenueHeartbeat> VenuesLoop::ListVenueHeartbeats() const {
  std::lock_guard<std::mutex> lock(data_mu_);
  std::vector<domain::VenueHeartbeat> out;
  out.reserve(last_heartbeats_.size());
  for (const auto& [venue, hb] : last_heartbeats_) {
    (void)venue;
    out.push_back(hb);
  }
  std::sort(out.begin(), out.end(), [](const domain::VenueHeartbeat& lhs,
                                       const domain::VenueHeartbeat& rhs) {
    return lhs.venue_id < rhs.venue_id;
  });
  return out;
}

std::optional<domain::VenueHeartbeat> VenuesLoop::GetVenueHeartbeat(
    const std::string& venue_id) const {
  std::lock_guard<std::mutex> lock(data_mu_);
  const auto it = last_heartbeats_.find(venue_id);
  if (it == last_heartbeats_.end()) return std::nullopt;
  return it->second;
}

std::optional<VenueRuntimeMetrics> VenuesLoop::GetVenueRuntimeMetrics(
    const std::string& venue_id) const {
  std::lock_guard<std::mutex> lock(data_mu_);
  const auto it = runtime_metrics_by_venue_.find(venue_id);
  if (it == runtime_metrics_by_venue_.end()) return std::nullopt;
  return it->second;
}

std::optional<fob::venue::v1::VenueSnapshot> VenuesLoop::GetLastVenueSnapshot(
    const std::string& venue_id) const {
  std::lock_guard<std::mutex> lock(data_mu_);
  const auto it = last_snapshots_.find(venue_id);
  if (it == last_snapshots_.end()) return std::nullopt;
  return it->second;
}

std::optional<fob::venue::v1::VenueLiquidityCurve> VenuesLoop::GetLastVenueCurve(
    const std::string& venue_id) const {
  std::lock_guard<std::mutex> lock(data_mu_);
  const auto it = curve_history_.find(venue_id);
  if (it == curve_history_.end() || it->second.empty()) return std::nullopt;
  return it->second.back();
}

std::vector<fob::venue::v1::VenueSnapshot> VenuesLoop::GetVenueSnapshots(
    const std::string& venue_id, const std::size_t limit) const {
  std::lock_guard<std::mutex> lock(data_mu_);
  const auto it = snapshot_history_.find(venue_id);
  if (it == snapshot_history_.end()) return {};
  const auto& history = it->second;
  const std::size_t count = std::min(limit, history.size());
  return {history.end() - static_cast<std::ptrdiff_t>(count), history.end()};
}

std::vector<fob::venue::v1::VenueLiquidityCurve> VenuesLoop::GetVenueCurves(
    const std::string& venue_id, const std::size_t limit) const {
  std::lock_guard<std::mutex> lock(data_mu_);
  const auto it = curve_history_.find(venue_id);
  if (it == curve_history_.end()) return {};
  const auto& history = it->second;
  const std::size_t count = std::min(limit, history.size());
  return {history.end() - static_cast<std::ptrdiff_t>(count), history.end()};
}

std::vector<fob::orders::v1::SyntheticFlowOrder> VenuesLoop::GetVenueSynthetics(
    const std::string& venue_id, const std::size_t limit) const {
  std::lock_guard<std::mutex> lock(data_mu_);
  const auto it = synthetic_history_.find(venue_id);
  if (it == synthetic_history_.end()) return {};
  const auto& history = it->second;
  const std::size_t count = std::min(limit, history.size());
  return {history.end() - static_cast<std::ptrdiff_t>(count), history.end()};
}

void VenuesLoop::start() {
  running_.store(true);
  connect_and_subscribe_defaults();
  t_md_ = std::thread([this] { md_publish_loop(); });
  t_exec_ = std::thread([this] { exec_consume_loop(); });
  t_sim_config_ = std::thread([this] { sim_config_consume_loop(); });
}

void VenuesLoop::stop() {
  running_.store(false);
  if (t_md_.joinable()) t_md_.join();
  if (t_exec_.joinable()) t_exec_.join();
  if (t_sim_config_.joinable()) t_sim_config_.join();
}

// F-20 Phase 4 — consume `sim.config` (SimConfigEvent) and apply each to the
// in-memory SimSessionRegistry for hot reload. Read-only side effect: only the
// registry changes; existing LIVE flows are untouched until the router fork
// (next wiring step) reads it. auto_offset_reset defaults to "earliest" so a
// restarted consumer replays the config stream and rebuilds the registry
// (UPSERT/DELETE are idempotent and order-stable).
void VenuesLoop::sim_config_consume_loop() {
  const std::string group = cex::common::Env::get_string(
      "VENUES_SIM_CONFIG_CONSUMER_GROUP", "venues_sim_config");
  cex::common::log_json("INFO", "Venues sim.config consumer loop starting",
                        {{"group_id", group}, {"topic", "sim.config"}});
  cex::common::KafkaConsumer consumer(
      {.brokers = brokers_,
       .group_id = group,
       .client_id = "venues_sim_config",
       .enable_auto_commit = false,
       .auto_offset_reset = cex::common::Env::get_string(
           "VENUES_SIM_CONFIG_AUTO_OFFSET_RESET", "earliest")});
  if (!consumer.subscribe({"sim.config"})) {
    cex::common::log_json("ERROR", "Venues sim.config consumer subscribe failed");
    return;
  }

  while (running_.load()) {
    bool ok = consumer.poll_once(
        500, [this](const std::string& topic, const std::string& key,
                    const std::string& payload) {
          (void)topic;
          (void)key;
          fob::sim::v1::SimConfigEvent evt;
          if (!cex::common::from_bytes(payload, evt)) {
            cex::common::log_json("ERROR", "Failed to parse SimConfigEvent");
            return;
          }
          const SimConfigAction action =
              ApplySimConfigEvent(evt, sim_session_registry_);
          cex::common::log_json(
              "INFO", "Applied sim.config event",
              {{"service", "venues"},
               {"component", "venue_sim_router"},
               {"action", ToString(action)},
               {"sim_session_id", evt.session().sim_session_id()},
               {"routing_mode",
                std::to_string(static_cast<int>(evt.session().routing_mode()))},
               {"active_sessions",
                std::to_string(sim_session_registry_.ActiveCount())}});
        });
    if (!ok) {
      cex::common::log_json("ERROR", "Venues sim.config consumer poll error");
      break;
    }
  }
  cex::common::log_json("INFO", "Venues sim.config consumer loop stopped");
}

void VenuesLoop::PublishSimExecution(
    const fob::execution::v1::ExecutionIntent& intent,
    const RouteDecision& decision) {
  // Cached live LOB for the venue. nullptr -> the engine rejects with
  // SIM_NO_LIQUIDITY (no crash), which is the correct outcome (cannot
  // simulate without a book).
  std::optional<fob::venue::v1::VenueSnapshot> snap =
      GetLastVenueSnapshot(intent.venue());
  uint32_t lob_age_ms = 0;
  if (snap.has_value()) {
    const auto now = cex::common::now_ts();
    const int64_t now_ms =
        static_cast<int64_t>(now.seconds()) * 1000 + now.nanos() / 1000000;
    const auto& ts = snap->meta().ts_event();
    const int64_t snap_ms =
        static_cast<int64_t>(ts.seconds()) * 1000 + ts.nanos() / 1000000;
    if (now_ms > snap_ms) {
      lob_age_ms = static_cast<uint32_t>(
          std::min<int64_t>(now_ms - snap_ms, 0xFFFFFFFF));
    }
  }

  SimExecutionInputs inputs;
  inputs.intent = intent;
  inputs.snapshot = snap.has_value() ? &*snap : nullptr;
  inputs.lob_age_ms = lob_age_ms;
  const std::string& seed_key = intent.client_order_id().empty()
                                    ? intent.intent_id()
                                    : intent.client_order_id();
  inputs.rng_seed = static_cast<uint64_t>(std::hash<std::string>{}(seed_key));

  const SimExecutionOutput out = sim_assembler_.Assemble(inputs, decision);

  // Async venue latency: apply the sampled delay before publishing, capped to
  // avoid pathologically blocking the single exec consumer thread. MVP
  // simplification — a production impl would use a delayed-publish queue.
  const uint32_t cap_ms = static_cast<uint32_t>(
      std::max(0, cex::common::Env::get_int("VENUES_SIM_MAX_DELAY_MS", 1000)));
  const uint32_t delay_ms = std::min(out.annotation.latency_sample_ms(), cap_ms);
  if (delay_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }

  if (kafka_publisher_ != nullptr) {
    const std::string key = intent.hedge_flow_id().empty()
                                ? intent.intent_id()
                                : intent.hedge_flow_id();
    (void)kafka_publisher_->Publish("sim.execution.venue", key,
                                    cex::common::to_bytes(out.report));
    (void)kafka_publisher_->Publish("sim.execution.annotations",
                                    out.report.report_id(),
                                    cex::common::to_bytes(out.annotation));
  }

  cex::common::log_json(
      "INFO", "Produced sim execution report",
      {{"service", "venues"},
       {"component", "venue_sim_router"},
       {"stage", "publish_sim_execution"},
       {"topic", "sim.execution.venue"},
       {"intent_id", intent.intent_id()},
       {"sim_session_id", decision.sim_session_id},
       {"report_id", out.report.report_id()},
       {"status", std::to_string(static_cast<int>(out.report.status()))},
       {"applied_latency_ms", std::to_string(delay_ms)}});

  // F-20 DoD-5 — SimAlert emission. Map engine reject reasons (set by
  // VenueSimulator) to fob.sim.v1.SimAlertType and publish to `sim.alerts`.
  // STALE_LOB / SIM_TIMEOUT / SIM_NO_LIQUIDITY -> alert; non-rejected fills
  // emit nothing. RANDOM_REJECT_SPIKE is rate-window observability and is
  // not derivable from a single report — left for a future counter.
  if (out.report.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED &&
      kafka_publisher_ != nullptr) {
    fob::sim::v1::SimAlertType alert_type =
        fob::sim::v1::SIM_ALERT_TYPE_UNSPECIFIED;
    const std::string& reason = out.report.error().code();
    if (reason == "SIM_STALE_LOB") {
      alert_type = fob::sim::v1::SIM_ALERT_TYPE_STALE_LOB;
    } else if (reason == "SIM_TIMEOUT") {
      alert_type = fob::sim::v1::SIM_ALERT_TYPE_SIM_TIMEOUT;
    } else if (reason == "SIM_NO_LIQUIDITY") {
      alert_type = fob::sim::v1::SIM_ALERT_TYPE_LOB_SOURCE_DOWN;
    }
    if (alert_type != fob::sim::v1::SIM_ALERT_TYPE_UNSPECIFIED) {
      fob::sim::v1::SimAlert alert;
      auto* ameta = alert.mutable_meta();
      ameta->set_event_id(cex::common::uuid_v4());
      *ameta->mutable_ts_event() = cex::common::now_ts();
      ameta->set_source("venue-simulator");
      ameta->set_partition_key(decision.sim_session_id);
      alert.set_alert_id(cex::common::uuid_v4());
      alert.set_type(alert_type);
      alert.set_sim_session_id(decision.sim_session_id);
      alert.set_venue_id(intent.venue());
      alert.set_symbol(RoutingSymbol(intent.instrument()));
      alert.set_message(out.report.error().message());
      *alert.mutable_ts() = cex::common::now_ts();
      (void)kafka_publisher_->Publish("sim.alerts", decision.sim_session_id,
                                      cex::common::to_bytes(alert));
      cex::common::log_json(
          "WARN", "Published sim alert",
          {{"service", "venues"},
           {"component", "venue_sim_router"},
           {"stage", "publish_sim_alert"},
           {"topic", "sim.alerts"},
           {"alert_id", alert.alert_id()},
           {"type", std::to_string(static_cast<int>(alert_type))},
           {"sim_session_id", decision.sim_session_id},
           {"reason", reason}});
    }
  }
}

void VenuesLoop::connect_and_subscribe_defaults() {
  std::lock_guard<std::mutex> lock(runtime_mu_);
  for (auto& adapter : adapters_) {
    std::string routing_mode = "auto";
    domain::VenueSubscription subscription = default_subscription_;
    {
      std::lock_guard<std::mutex> cfg_lock(config_mu_);
      const auto cfg_it = venue_configs_.find(adapter->VenueId());
      if (cfg_it != venue_configs_.end()) {
        routing_mode = normalize_routing_mode(cfg_it->second.routing_mode);
        if (!cfg_it->second.venue_symbol.empty()) {
          subscription.venue_symbol = cfg_it->second.venue_symbol;
        }
        subscription.depth_levels =
            std::max<std::size_t>(1, static_cast<std::size_t>(cfg_it->second.depth_levels));
      }
    }

    if (!adapter->Connect()) {
      cex::common::log_json("ERROR", "Venue adapter connect failed",
                            {{"venue", adapter->VenueId()}});
      (void)observability_.PublishError(
          adapter->VenueId(),
          "Venue adapter connect failed",
          {{"stage", "connect"}});
      continue;
    }

    if (!adapter->Subscribe({subscription})) {
      cex::common::log_json("ERROR", "Venue adapter subscribe failed",
                            {{"venue", adapter->VenueId()}});
      (void)observability_.PublishError(
          adapter->VenueId(),
          "Venue adapter subscribe failed",
          {{"stage", "subscribe"}});
    }

    const auto hb = adapter->Heartbeat();
    {
      std::lock_guard<std::mutex> data_lock(data_mu_);
      last_heartbeats_[hb.venue_id] = hb;
    }
    (void)observability_.PublishStatus(hb, "startup", routing_mode);
  }
}

domain::VenueAdapter* VenuesLoop::find_adapter(const std::string& venue_id) {
  if (adapters_.empty()) return nullptr;

  auto is_adapter_eligible = [this](domain::VenueAdapter* adapter,
                                    const bool enforce_health_check) {
    if (adapter == nullptr) return false;
    {
      std::lock_guard<std::mutex> cfg_lock(config_mu_);
      const auto it = venue_configs_.find(adapter->VenueId());
      if (it != venue_configs_.end()) {
        if (!it->second.is_active) return false;
        if (normalize_routing_mode(it->second.routing_mode) == "watch") return false;
      }
    }
    if (!enforce_health_check) return true;
    const auto hb = adapter->Heartbeat();
    return hb.status != domain::VenueConnectionStatus::kDisconnected;
  };

  if (!venue_id.empty()) {
    for (auto& adapter : adapters_) {
      if (adapter->VenueId() != venue_id) continue;
      return is_adapter_eligible(adapter.get(), false) ? adapter.get() : nullptr;
    }
    return nullptr;
  }

  const std::size_t count = adapters_.size();
  std::size_t start = next_exec_adapter_idx_ % count;
  for (std::size_t offset = 0; offset < count; ++offset) {
    const std::size_t idx = (start + offset) % count;
    if (!is_adapter_eligible(adapters_[idx].get(), true)) continue;
    next_exec_adapter_idx_ = (idx + 1) % count;
    return adapters_[idx].get();
  }

  for (std::size_t offset = 0; offset < count; ++offset) {
    const std::size_t idx = (start + offset) % count;
    if (!is_adapter_eligible(adapters_[idx].get(), false)) continue;
    next_exec_adapter_idx_ = (idx + 1) % count;
    return adapters_[idx].get();
  }

  return nullptr;
}

void VenuesLoop::md_publish_loop() {
  using namespace std::chrono;

  const int poll_interval_ms = std::max(
      1000, cex::common::Env::get_int("VENUES_MD_LOOP_INTERVAL_MS", 5000));
  if (poll_interval_ms >= 5000) {
    cex::common::log_json("INFO",
                          "Venues market-data loop uses real-venue polling cadence",
                          {{"interval_ms", std::to_string(poll_interval_ms)}});
  }

  while (running_.load()) {
    for (auto& adapter : adapters_) {
      try {
        bool venue_active = true;
        std::string routing_mode = "auto";
        domain::VenueSnapshotRequest request = default_snapshot_request_;
        uint32_t stale_threshold_ms = static_cast<uint32_t>(std::max<int64_t>(
            1, snapshot_config_.status.stale_threshold_ms));
        std::string requested_curve_level = curve_config_.level;
        bool synthetic_enabled = curve_config_.synthetic.enabled;
        {
          std::lock_guard<std::mutex> cfg_lock(config_mu_);
          const auto cfg_it = venue_configs_.find(adapter->VenueId());
          if (cfg_it != venue_configs_.end()) {
            venue_active = cfg_it->second.is_active;
            routing_mode = normalize_routing_mode(cfg_it->second.routing_mode);
            if (!cfg_it->second.venue_symbol.empty()) {
              request.venue_symbol = cfg_it->second.venue_symbol;
            }
            request.depth_levels =
                std::max<std::size_t>(1, static_cast<std::size_t>(cfg_it->second.depth_levels));
            stale_threshold_ms = std::max<uint32_t>(1, cfg_it->second.stale_threshold_ms);
            if (!cfg_it->second.curve_level.empty()) {
              requested_curve_level = cfg_it->second.curve_level;
            }
            synthetic_enabled = cfg_it->second.synthetic_enabled;
          }
        }

        const auto heartbeat = adapter->Heartbeat();
        if (!venue_active) {
          auto muted_hb = heartbeat;
          muted_hb.status = domain::VenueConnectionStatus::kDisconnected;
          muted_hb.circuit_breaker_reason = "disabled_by_operator";
          muted_hb.timestamp = cex::common::now_ts();
          {
            std::lock_guard<std::mutex> data_lock(data_mu_);
            last_heartbeats_[muted_hb.venue_id] = muted_hb;
          }
          (void)observability_.PublishStatus(muted_hb, "disabled", "disable");
          continue;
        }

        {
          std::lock_guard<std::mutex> data_lock(data_mu_);
          last_heartbeats_[heartbeat.venue_id] = heartbeat;
        }
        (void)observability_.PublishStatus(heartbeat, "poll", routing_mode);
        if (heartbeat.status == domain::VenueConnectionStatus::kDisconnected &&
            !adapter->Reconnect()) {
          cex::common::log_json("WARN", "Adapter reconnect failed",
                                {{"venue", adapter->VenueId()}});
          (void)observability_.PublishError(
              adapter->VenueId(),
              "Venue adapter reconnect failed",
              {{"stage", "heartbeat_reconnect"}});
          continue;
        }

        const std::optional<domain::VenueRawSnapshot> snapshot =
            adapter->RequestSnapshot(request);
        if (!snapshot.has_value()) {
          cex::common::log_json("WARN", "Adapter returned empty snapshot",
                                {{"venue", adapter->VenueId()}});
          (void)observability_.PublishTraffic(
              adapter->VenueId(),
              "snapshot",
              0,
              false,
              "inbound",
              "empty_or_unavailable");
          continue;
        }

        (void)observability_.PublishTraffic(
            snapshot->venue_id,
            "snapshot",
            approx_snapshot_payload_bytes(*snapshot),
            true,
            "inbound",
            domain::ToString(snapshot->status));

        publish_raw_order_book(&producer_, *snapshot);
        publish_raw_symbol_meta(&producer_, *snapshot);
        publish_raw_ticker(&producer_, *snapshot);
        fob::venue::v1::VenueSnapshot normalized_snapshot;
        SnapshotProducer::PublishOptions snapshot_options;
        snapshot_options.stale_threshold_ms = static_cast<int64_t>(stale_threshold_ms);
        const bool snapshot_published = snapshot_producer_ != nullptr &&
            snapshot_producer_->Publish(*snapshot, &normalized_snapshot, snapshot_options);
        if (snapshot_published && liquidity_curve_producer_ != nullptr) {
          if (normalized_snapshot.status() == "empty") {
            (void)observability_.PublishMetric(
                normalized_snapshot.venue_id(),
                "venue.empty_snapshots",
                1.0,
                {{"symbol", normalized_snapshot.instrument().symbol()}});
          }

          {
            std::lock_guard<std::mutex> data_lock(data_mu_);
            last_snapshots_[normalized_snapshot.venue_id()] = normalized_snapshot;
            push_bounded(&snapshot_history_[normalized_snapshot.venue_id()],
                         normalized_snapshot, history_capacity_);
            ++total_snapshots_by_venue_[normalized_snapshot.venue_id()];
            if (normalized_snapshot.status() == "stale") {
              ++stale_snapshots_by_venue_[normalized_snapshot.venue_id()];
            }
          }

          double snapshots_per_sec = 0.0;
          double stale_rate = 0.0;
          {
            std::lock_guard<std::mutex> data_lock(data_mu_);
            const auto total = static_cast<double>(
                total_snapshots_by_venue_[normalized_snapshot.venue_id()]);
            const auto stale = static_cast<double>(
                stale_snapshots_by_venue_[normalized_snapshot.venue_id()]);
            const auto elapsed_ms = std::max<int64_t>(
                1,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - md_started_at_)
                    .count());
            snapshots_per_sec = total * 1000.0 / static_cast<double>(elapsed_ms);
            stale_rate = total > 0.0 ? stale / total : 0.0;
            auto& metrics = runtime_metrics_by_venue_[normalized_snapshot.venue_id()];
            metrics.snapshots_per_sec = snapshots_per_sec;
            metrics.stale_rate = stale_rate;
          }
          (void)observability_.PublishMetric(
              normalized_snapshot.venue_id(),
              "venue.snapshots_per_sec",
              snapshots_per_sec,
              {{"symbol", normalized_snapshot.instrument().symbol()}});
          (void)observability_.PublishMetric(
              normalized_snapshot.venue_id(),
              "venue.stale_rate",
              stale_rate,
              {{"symbol", normalized_snapshot.instrument().symbol()}});

          fob::venue::v1::VenueLiquidityCurve published_curve;
          std::vector<fob::orders::v1::SyntheticFlowOrder> published_synthetics;
          const std::string normalized_curve_level =
              normalize_curve_level(requested_curve_level);
          if (normalized_curve_level == "OFF") {
            std::lock_guard<std::mutex> data_lock(data_mu_);
            auto& metrics = runtime_metrics_by_venue_[normalized_snapshot.venue_id()];
            metrics.last_curve_requested_level = "OFF";
            metrics.last_curve_effective_level = "OFF";
            metrics.last_curve_degradation_reason = "disabled_by_config";
            metrics.last_curve_quality_action = "disabled";
            metrics.last_curve_confidence = 0.0;
            continue;
          }
          LiquidityCurveProducer::PublishOptions curve_options;
          curve_options.requested_level = normalized_curve_level;
          curve_options.synthetic_enabled = synthetic_enabled;
          const bool curve_published = liquidity_curve_producer_->Publish(
              normalized_snapshot, &published_curve, &published_synthetics, curve_options);
          if (!curve_published) {
            cex::common::log_json("WARN", "Failed to publish venue liquidity outputs",
                                  {{"venue", normalized_snapshot.venue_id()},
                                   {"symbol", normalized_snapshot.instrument().symbol()}});
            (void)observability_.PublishError(
                normalized_snapshot.venue_id(),
                "Failed to publish venue liquidity outputs",
                {{"symbol", normalized_snapshot.instrument().symbol()}});
          } else {
            std::lock_guard<std::mutex> data_lock(data_mu_);
            push_bounded(&curve_history_[published_curve.venue_id()],
                         published_curve, history_capacity_);
            auto& metrics = runtime_metrics_by_venue_[published_curve.venue_id()];
            metrics.last_curve_confidence = published_curve.confidence();
            metrics.last_curve_effective_level = published_curve.level();
            const auto& tags = published_curve.meta().tags();
            const auto latency_it = tags.find("build_latency_ms");
            if (latency_it != tags.end()) {
              char* end = nullptr;
              const double latency = std::strtod(latency_it->second.c_str(), &end);
              if (end != nullptr && *end == '\0') {
                metrics.last_curve_build_latency_ms = latency;
              }
            }
            const auto requested_it = tags.find("requested_level");
            if (requested_it != tags.end()) {
              metrics.last_curve_requested_level = requested_it->second;
            }
            const auto effective_it = tags.find("effective_level");
            if (effective_it != tags.end()) {
              metrics.last_curve_effective_level = effective_it->second;
            }
            const auto reason_it = tags.find("degradation_reason");
            if (reason_it != tags.end()) {
              metrics.last_curve_degradation_reason = reason_it->second;
            }
            const auto quality_it = tags.find("quality_action");
            if (quality_it != tags.end()) {
              metrics.last_curve_quality_action = quality_it->second;
            }
            for (const auto& order : published_synthetics) {
              push_bounded(&synthetic_history_[order.venue_id()],
                           order, history_capacity_);
            }
          }
        }
      } catch (const std::exception& ex) {
        cex::common::log_json("ERROR", "Venue adapter poll iteration failed",
                              {{"venue", adapter->VenueId()},
                               {"error", ex.what()}});
        (void)observability_.PublishError(
            adapter->VenueId(),
            "Venue adapter poll iteration failed",
            {{"error", ex.what()}});
      }
    }

    std::this_thread::sleep_for(milliseconds(poll_interval_ms));
  }
}

void VenuesLoop::exec_consume_loop() {
  cex::common::log_json(
      "INFO",
      "Venues execution consumer loop starting",
      {{"group_id", cex::common::Env::get_string(
                        "VENUES_EXEC_CONSUMER_GROUP", "venues_exec")},
       {"auto_offset_reset", cex::common::Env::get_string(
                                 "VENUES_EXEC_AUTO_OFFSET_RESET", "latest")}});
  cex::common::KafkaConsumer exec_consumer(
      {.brokers = brokers_,
       .group_id = cex::common::Env::get_string(
           "VENUES_EXEC_CONSUMER_GROUP", "venues_exec"),
       .client_id = "venues_exec",
       .enable_auto_commit = false,
       .auto_offset_reset = cex::common::Env::get_string(
           "VENUES_EXEC_AUTO_OFFSET_RESET", "latest")});
  if (!exec_consumer.subscribe({"execution.intents"})) {
    cex::common::log_json("ERROR", "Venues execution consumer subscribe failed");
    return;
  }

  while (running_.load()) {
    bool ok = exec_consumer.poll_once(500, [this](const std::string& topic,
                                                  const std::string& key,
                                                  const std::string& payload) {
      fob::execution::v1::ExecutionIntent intent;
      if (!cex::common::from_bytes(payload, intent)) {
        cex::common::log_json("ERROR", "Failed to parse ExecutionIntent");
        (void)observability_.PublishError(
            "unknown",
            "Failed to parse ExecutionIntent",
            {{"topic", topic},
             {"key", key},
             {"payload_bytes", std::to_string(payload.size())}});
        return;
      }

      const std::string venue_for_event = intent.venue().empty() ? "default" : intent.venue();
      cex::common::log_json(
          "INFO",
          "Venues execution consumer received intent",
          {{"service", "venues"},
           {"component", "venue_execution_adapter"},
           {"participant", "Venue Execution Adapter"},
           {"stage", "consume_execution_intent"},
           {"intent_id", intent.intent_id()},
           {"venue", venue_for_event},
           {"symbol", intent.instrument().symbol()},
           {"side", std::to_string(static_cast<int>(intent.side()))},
           {"target_qty",
            intent.has_target_qty()
                ? cex::common::Decimal::from_proto(intent.target_qty()).to_string()
                : "0"},
           {"limit_price",
            intent.has_limit_price()
                ? cex::common::Decimal::from_proto(intent.limit_price()).to_string()
                : "0"},
           {"venue_symbol", intent.venue_symbol()},
           {"batch_id", intent.batch_id()},
           {"client_order_id", intent.client_order_id()},
           {"topic", topic},
           {"key", key},
           {"payload_bytes", std::to_string(payload.size())},
           {"source_file", "cpp/venues/src/app/venues_loop.cpp"}});
      (void)observability_.PublishTraffic(
          venue_for_event,
          "execution.intents",
          payload.size(),
          true,
          "inbound",
          intent.intent_id());

      // F-20 Phase 4 — SIM/SHADOW routing decision. With no active SimSession
      // governing this (venue, instrument) the registry resolves to LIVE_ONLY,
      // so this is a no-op for the existing live path (opt-in by construction).
      const RouteDecision sim_decision =
          sim_router_.Decide(intent.venue(), RoutingSymbol(intent.instrument()));
      if (sim_decision.mode == fob::sim::v1::ROUTING_MODE_SIM_ONLY) {
        // Simulate only: no real EVC call, no live PG writes, nothing on
        // execution.venue. The sim report goes to the isolated sim.* topics.
        PublishSimExecution(intent, sim_decision);
        return;
      }

      // F-12 / IN-008 DoD-4 (PR-F12-3a): persist HedgeFlow + ChildOrder
      // BEFORE execution starts so PG reflects what venues is about to do
      // even if the adapter call hangs. Idempotent (ON CONFLICT DO NOTHING).
      if (hedgeflow_repo_ != nullptr) {
        (void)hedgeflow_repo_->InsertOpen(intent);
      }
      if (child_order_repo_ != nullptr) {
        (void)child_order_repo_->InsertPending(intent);
      }

      fob::execution::v1::ExecutionReport rep;
      try {
        rep = execute_on_venue_.Run(intent, find_adapter(intent.venue()));
      } catch (const std::exception& ex) {
        rep = make_execution_exception_report(
            intent, "EXECUTION_EXCEPTION", ex.what());
        cex::common::log_json(
            "ERROR",
            "Execution intent handling threw std::exception",
            {{"intent_id", intent.intent_id()},
             {"venue", venue_for_event},
             {"error", ex.what()}});
      } catch (...) {
        rep = make_execution_exception_report(
            intent, "EXECUTION_EXCEPTION", "unknown exception");
        cex::common::log_json(
            "ERROR",
            "Execution intent handling threw unknown exception",
            {{"intent_id", intent.intent_id()},
             {"venue", venue_for_event}});
      }

      // F-12 / PR-F12-5: propagate hedge_flow_id from intent to report so
      // PostgresHedgeflowRepository::ApplyReport finds the row inserted by
      // InsertOpen. Without this, real F-12 hedge intents fall through to
      // intent_id-fallback which has a "|intent" suffix mismatch.
      if (rep.hedge_flow_id().empty() && !intent.hedge_flow_id().empty()) {
        rep.set_hedge_flow_id(intent.hedge_flow_id());
      }

      if (rep.has_error()) {
        (void)observability_.PublishError(
            rep.venue().empty() ? venue_for_event : rep.venue(),
            rep.error().message().empty() ? "ExecuteOnVenue failed"
                                          : rep.error().message(),
            {{"intent_id", intent.intent_id()},
             {"code", rep.error().code()}});
      }

      try {
        const fob::execution::v1::ExecutionReport out =
            infra::ExecutionReportProducer::Normalize(rep);
        if (liquidity_curve_producer_ != nullptr) {
          liquidity_curve_producer_->ObserveExecution(intent, out);
        }
        const std::string rep_payload = cex::common::to_bytes(out);
        if (execution_report_producer_ != nullptr) {
          (void)execution_report_producer_->Publish(out);
        }

        // F-12 / IN-008 DoD-4 (PR-F12-3a): apply terminal state to PG.
        // Order matters: child_orders FIRST (per-row update), then
        // hedgeflows (aggregate accumulator + status promotion).
        if (child_order_repo_ != nullptr) {
          (void)child_order_repo_->ApplyReport(out);
        }
        if (hedgeflow_repo_ != nullptr) {
          (void)hedgeflow_repo_->ApplyReport(out);
        }

        (void)observability_.PublishTraffic(
            out.venue(),
            "execution.venue",
            rep_payload.size(),
            true,
            "outbound",
            out.intent_id());

        cex::common::log_json("INFO", "Produced execution report",
                              {{"service", "venues"},
                               {"component", "venue_execution_adapter"},
                               {"participant", "Venue Execution Adapter"},
                               {"stage", "publish_execution_report"},
                               {"topic", "execution.venue"},
                               {"intent_id", intent.intent_id()},
                               {"report_id", out.report_id()},
                               {"venue", out.venue()},
                               {"symbol", out.instrument().symbol()},
                               {"status", std::to_string(out.status())},
                               {"filled_qty",
                                out.has_filled_qty()
                                    ? cex::common::Decimal::from_proto(out.filled_qty()).to_string()
                                    : "0"},
                               {"remaining_qty",
                                out.has_remaining_qty()
                                    ? cex::common::Decimal::from_proto(out.remaining_qty()).to_string()
                                    : "0"},
                               {"average_price",
                                out.has_average_price()
                                    ? cex::common::Decimal::from_proto(out.average_price()).to_string()
                                    : "0"},
                               {"error_code", out.has_error() ? out.error().code() : ""},
                               {"source_file", "cpp/venues/src/app/venues_loop.cpp"}});
      } catch (const std::exception& ex) {
        cex::common::log_json(
            "ERROR",
            "Execution report post-processing threw std::exception",
            {{"intent_id", intent.intent_id()},
             {"venue", rep.venue().empty() ? venue_for_event : rep.venue()},
             {"error", ex.what()}});
      } catch (...) {
        cex::common::log_json(
            "ERROR",
            "Execution report post-processing threw unknown exception",
            {{"intent_id", intent.intent_id()},
             {"venue", rep.venue().empty() ? venue_for_event : rep.venue()}});
      }

      // F-20 SHADOW — after the LIVE send completed above, also run the
      // simulation and publish to the isolated sim.* topics (paired with the
      // LIVE report by client_order_id for the Divergence Service).
      if (sim_decision.mode == fob::sim::v1::ROUTING_MODE_SHADOW) {
        PublishSimExecution(intent, sim_decision);
      }
    });

    if (!ok) {
      cex::common::log_json("ERROR", "Venues execution consumer loop stopping after poll failure");
      break;
    }
  }
}

}  // namespace cex::venues::app
