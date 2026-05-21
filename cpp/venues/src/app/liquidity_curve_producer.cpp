#include "app/liquidity_curve_producer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"
#include "domain/amm_direct_fob.hpp"

namespace cex::venues::app {

namespace {

cex::common::Decimal TauSecondsFromMs(double tau_ms) {
  if (!std::isfinite(tau_ms) || tau_ms <= 0.0) {
    tau_ms = 1000.0;
  }
  const int64_t tau_us = static_cast<int64_t>(std::llround(tau_ms * 1000.0));
  // microseconds scale -> seconds.
  return cex::common::Decimal{
      .units = std::max<int64_t>(1, tau_us),
      .scale = 6,
  };
}

google::protobuf::Timestamp AddMilliseconds(
    google::protobuf::Timestamp ts,
    const int64_t millis) {
  const int64_t safe_millis = std::max<int64_t>(1, millis);
  ts.set_seconds(ts.seconds() + safe_millis / 1000);

  int64_t nanos = static_cast<int64_t>(ts.nanos()) +
                  (safe_millis % 1000) * 1000 * 1000;
  if (nanos >= 1000LL * 1000LL * 1000LL) {
    ts.set_seconds(ts.seconds() + nanos / (1000LL * 1000LL * 1000LL));
    nanos %= 1000LL * 1000LL * 1000LL;
  }
  ts.set_nanos(static_cast<int32_t>(nanos));
  return ts;
}

std::string CurvePartitionKey(const fob::venue::v1::VenueSnapshot& snapshot) {
  if (snapshot.has_meta() && !snapshot.meta().partition_key().empty()) {
    return snapshot.meta().partition_key();
  }
  return snapshot.venue_id() + "|" + snapshot.instrument().symbol();
}

double DecimalAsDouble(const cex::common::Decimal& value) {
  return static_cast<double>(value);
}

double ProtoDecimalAsDouble(const fob::common::v1::Decimal& value) {
  return static_cast<double>(cex::common::Decimal::from_proto(value));
}

double CurveQMax(const domain::DepthSideCurves& curves) {
  double q_max = 0.0;
  for (const auto& point : curves.s_of_q) {
    const double qty = DecimalAsDouble(point.qty);
    if (std::isfinite(qty) && qty > q_max) {
      q_max = qty;
    }
  }
  for (const auto& point : curves.p_of_q) {
    const double qty = DecimalAsDouble(point.qty);
    if (std::isfinite(qty) && qty > q_max) {
      q_max = qty;
    }
  }
  return q_max;
}

struct EffectiveTauMetrics {
  double base_tau_ms{1000.0};
  double effective_tau_ms{1000.0};
  double q_max{0.0};
  double volume_24h_qty{0.0};
  double raw_hourly_rate{0.0};
  double hourly_turnover_cap{0.0};
  bool capped_by_turnover{false};
};

EffectiveTauMetrics ComputeEffectiveTauMetrics(
    const fob::venue::v1::VenueSnapshot& snapshot,
    const domain::DepthSideCurves& bid_curve,
    const domain::DepthSideCurves& ask_curve,
    const double configured_tau_ms) {
  EffectiveTauMetrics metrics;
  metrics.base_tau_ms =
      (std::isfinite(configured_tau_ms) && configured_tau_ms > 0.0)
          ? configured_tau_ms
          : 1000.0;
  metrics.effective_tau_ms = metrics.base_tau_ms;
  metrics.q_max = std::max(CurveQMax(bid_curve), CurveQMax(ask_curve));
  if (!std::isfinite(metrics.q_max) || metrics.q_max <= 0.0) {
    metrics.q_max = 0.0;
    return metrics;
  }

  if (!snapshot.has_volume_24h()) {
    return metrics;
  }

  metrics.volume_24h_qty = ProtoDecimalAsDouble(snapshot.volume_24h());
  if (!std::isfinite(metrics.volume_24h_qty) || metrics.volume_24h_qty <= 0.0) {
    metrics.volume_24h_qty = 0.0;
    return metrics;
  }

  metrics.raw_hourly_rate = metrics.q_max * 3600000.0 / metrics.base_tau_ms;
  metrics.hourly_turnover_cap = metrics.volume_24h_qty;
  if (!std::isfinite(metrics.raw_hourly_rate) || metrics.raw_hourly_rate <= 0.0 ||
      !std::isfinite(metrics.hourly_turnover_cap) || metrics.hourly_turnover_cap <= 0.0 ||
      metrics.raw_hourly_rate <= metrics.hourly_turnover_cap) {
    return metrics;
  }

  const double capped_tau_ms = metrics.q_max * 3600000.0 / metrics.hourly_turnover_cap;
  if (std::isfinite(capped_tau_ms) && capped_tau_ms > metrics.effective_tau_ms) {
    metrics.effective_tau_ms = capped_tau_ms;
    metrics.capped_by_turnover = true;
  }
  return metrics;
}

enum class CurveBuildLevel : int {
  kOff = 0,
  kL1 = 1,
  kL2 = 2,
  kL3 = 3,
};

struct CurveQuality {
  double epsilon1{0.0};
  double epsilon2{0.0};
  double epsilon3{0.0};
  double confidence{0.0};
};

struct LegacyQualityMetrics {
  double epsilon1{0.0};
  double epsilon2{0.0};
  double epsilon3{0.0};
  double confidence{0.0};
};

enum class CurveQualityAction {
  kAccept,
  kDegrade,
  kDisable,
};

std::string NormalizeText(std::string level) {
  for (char& c : level) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return level;
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string InferLiquiditySource(const std::string& venue_id,
                                 const std::string& configured_source) {
  if (!configured_source.empty()) return configured_source;
  const std::string venue = LowerAscii(venue_id);
  if (venue.find("uniswap") != std::string::npos ||
      venue.find("dex") != std::string::npos) {
    return "dex_hedge";
  }
  return "cex_hedge";
}

CurveBuildLevel ParseLevel(const std::string& raw_level) {
  const std::string level = NormalizeText(raw_level);
  if (level == "OFF") return CurveBuildLevel::kOff;
  if (level == "L1") return CurveBuildLevel::kL1;
  if (level == "L3") return CurveBuildLevel::kL3;
  return CurveBuildLevel::kL2;
}

std::string LevelToString(const CurveBuildLevel level) {
  switch (level) {
    case CurveBuildLevel::kL1:
      return "L1";
    case CurveBuildLevel::kL2:
      return "L2";
    case CurveBuildLevel::kL3:
      return "L3";
    case CurveBuildLevel::kOff:
      return "OFF";
  }
  return "OFF";
}

std::string ImpactModelToString(const L3ImpactModel model) {
  switch (model) {
    case L3ImpactModel::kLinear:
      return "linear";
    case L3ImpactModel::kQuadratic:
      return "quadratic";
    case L3ImpactModel::kSqrt:
      return "sqrt";
  }
  return "linear";
}

CurveBuildLevel MinLevel(const CurveBuildLevel lhs, const CurveBuildLevel rhs) {
  return static_cast<int>(lhs) < static_cast<int>(rhs) ? lhs : rhs;
}

CurveBuildLevel DowngradeBySteps(CurveBuildLevel level, uint32_t steps) {
  int rank = static_cast<int>(level);
  rank = std::max(0, rank - static_cast<int>(steps));
  return static_cast<CurveBuildLevel>(rank);
}

bool IsStaleStatus(const std::string& status) {
  return NormalizeText(status) == "STALE";
}

bool IsOffStatus(const std::string& status) {
  const std::string normalized = NormalizeText(status);
  return normalized == "DISCONNECTED" || normalized == "EMPTY";
}

std::string TagOrEmpty(const fob::common::v1::EventMeta& meta,
                       const std::string& key) {
  const auto it = meta.tags().find(key);
  if (it == meta.tags().end()) return {};
  return it->second;
}

int64_t ParseI64(const std::string& text) {
  if (text.empty()) return 0;
  char* end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return 0;
  return static_cast<int64_t>(value);
}

uint64_t ParseU64(const std::string& text) {
  if (text.empty()) return 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return 0;
  return static_cast<uint64_t>(value);
}

bool ParseBool(const std::string& text, const bool fallback) {
  if (text.empty()) return fallback;
  const std::string normalized = LowerAscii(text);
  if (normalized == "1" || normalized == "true" || normalized == "yes") return true;
  if (normalized == "0" || normalized == "false" || normalized == "no") return false;
  return fallback;
}

std::vector<domain::VenuePoolTickLevel> ParseAmmTicks(const std::string& encoded_ticks) {
  std::vector<domain::VenuePoolTickLevel> out;
  if (encoded_ticks.empty()) return out;

  // Format from normalizer: "<tick>,<liq_units>,<liq_scale>;..."
  std::stringstream rows(encoded_ticks);
  std::string row;
  while (std::getline(rows, row, ';')) {
    if (row.empty()) continue;
    std::stringstream cols(row);
    std::string tick_text;
    std::string units_text;
    std::string scale_text;
    if (!std::getline(cols, tick_text, ',')) continue;
    if (!std::getline(cols, units_text, ',')) continue;
    if (!std::getline(cols, scale_text, ',')) continue;

    const int64_t tick = ParseI64(tick_text);
    const int64_t units = ParseI64(units_text);
    const int32_t scale = static_cast<int32_t>(std::max<int64_t>(0, ParseI64(scale_text)));
    out.push_back(domain::VenuePoolTickLevel{
        .tick = tick,
        .liquidity_net = cex::common::Decimal{units, scale},
    });
  }

  std::sort(out.begin(), out.end(),
            [](const domain::VenuePoolTickLevel& lhs,
               const domain::VenuePoolTickLevel& rhs) {
              return lhs.tick < rhs.tick;
            });
  return out;
}

bool IsAmmLikeVenueSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) {
  if (!snapshot.has_meta()) return false;
  const std::string venue_type = LowerAscii(TagOrEmpty(snapshot.meta(), "venue_type"));
  return venue_type == "amm" || venue_type == "dex";
}

std::optional<domain::VenuePoolState> TryExtractAmmPoolState(
    const fob::venue::v1::VenueSnapshot& snapshot) {
  if (!snapshot.has_meta()) return std::nullopt;
  if (!IsAmmLikeVenueSnapshot(snapshot)) return std::nullopt;

  const auto& meta = snapshot.meta();
  if (!ParseBool(TagOrEmpty(meta, "amm.pool_state.present"), false)) {
    return std::nullopt;
  }

  domain::VenuePoolState pool;
  pool.pool_address = TagOrEmpty(meta, "amm.pool_address");
  pool.sqrt_price_x96 = TagOrEmpty(meta, "amm.sqrt_price_x96");
  pool.tick = ParseI64(TagOrEmpty(meta, "amm.tick"));
  pool.liquidity = TagOrEmpty(meta, "amm.liquidity");
  pool.block_number = ParseU64(TagOrEmpty(meta, "amm.block_number"));
  pool.finalized = ParseBool(TagOrEmpty(meta, "amm.finalized"), true);
  pool.ticks = ParseAmmTicks(TagOrEmpty(meta, "amm.ticks"));

  if (pool.ticks.empty() || pool.liquidity.empty()) {
    return std::nullopt;
  }
  return pool;
}

bool IsExecutionFailure(const fob::execution::v1::ExecutionReport& report) {
  if (report.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED ||
      report.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED) {
    return true;
  }
  return report.has_error() &&
         (!report.error().code().empty() ||
          !report.error().message().empty() ||
          !report.error().details().empty());
}

double ClampFinite(const double value, const double lo, const double hi) {
  if (!std::isfinite(value)) return lo;
  return std::clamp(value, lo, hi);
}

double SafePositiveScale(const double value, const double fallback) {
  if (!std::isfinite(value) || value <= 0.0) return fallback;
  return value;
}

double MinConfidenceForLevel(const CurveBuildLevel level,
                             const CurveDegradationConfig& config) {
  switch (level) {
    case CurveBuildLevel::kL3:
      return ClampFinite(config.min_l3_confidence, 0.0, 1.0);
    case CurveBuildLevel::kL2:
      return ClampFinite(config.min_l2_confidence, 0.0, 1.0);
    case CurveBuildLevel::kL1:
      return ClampFinite(config.min_l1_confidence, 0.0, 1.0);
    case CurveBuildLevel::kOff:
      return 1.0;
  }
  return 1.0;
}

CurveQualityThresholds NormalizeQualityThresholds(CurveQualityThresholds thresholds) {
  const CurveQualityThresholds defaults{};
  const auto sanitize = [](const double value, const double fallback) {
    if (!std::isfinite(value) || value < 0.0) return fallback;
    return value;
  };

  thresholds.epsilon1_degrade = sanitize(thresholds.epsilon1_degrade, defaults.epsilon1_degrade);
  thresholds.epsilon2_degrade = sanitize(thresholds.epsilon2_degrade, defaults.epsilon2_degrade);
  thresholds.epsilon3_degrade = sanitize(thresholds.epsilon3_degrade, defaults.epsilon3_degrade);

  thresholds.epsilon1_disable = sanitize(thresholds.epsilon1_disable, defaults.epsilon1_disable);
  thresholds.epsilon2_disable = sanitize(thresholds.epsilon2_disable, defaults.epsilon2_disable);
  thresholds.epsilon3_disable = sanitize(thresholds.epsilon3_disable, defaults.epsilon3_disable);

  thresholds.epsilon1_disable = std::max(thresholds.epsilon1_disable, thresholds.epsilon1_degrade);
  thresholds.epsilon2_disable = std::max(thresholds.epsilon2_disable, thresholds.epsilon2_degrade);
  thresholds.epsilon3_disable = std::max(thresholds.epsilon3_disable, thresholds.epsilon3_degrade);
  return thresholds;
}

SyntheticFlowOrderConfig NormalizeSyntheticConfig(SyntheticFlowOrderConfig config) {
  if (config.topic.empty()) {
    config.topic = "venue.synthetic";
  }
  config.ttl_ms = std::max<int64_t>(1, config.ttl_ms);
  config.price_scale = std::clamp(config.price_scale, 0, 12);
  config.quantity_scale = std::clamp(config.quantity_scale, 0, 12);
  return config;
}

LiquidityCurveInputConfig NormalizeInputConfig(LiquidityCurveInputConfig config) {
  if (config.min_qty.units < 0 || config.min_qty.scale < 0) {
    config.min_qty = cex::common::Decimal::zero();
  }
  config.fee_adjusted_price_scale =
      std::clamp(config.fee_adjusted_price_scale, 0, 12);
  return config;
}

std::string QualityActionToString(const CurveQualityAction action,
                                  const bool gating_enabled) {
  if (!gating_enabled) return "none";
  switch (action) {
    case CurveQualityAction::kAccept:
      return "accept";
    case CurveQualityAction::kDegrade:
      return "degrade";
    case CurveQualityAction::kDisable:
      return "disable";
  }
  return "none";
}

CurveQualityAction DecideQualityAction(const LegacyQualityMetrics& metrics,
                                       const CurveQualityThresholds& thresholds,
                                       const bool gating_enabled) {
  if (!gating_enabled) return CurveQualityAction::kAccept;

  const bool disable =
      metrics.epsilon1 >= thresholds.epsilon1_disable ||
      metrics.epsilon2 >= thresholds.epsilon2_disable ||
      metrics.epsilon3 >= thresholds.epsilon3_disable;
  if (disable) return CurveQualityAction::kDisable;

  const bool degrade =
      metrics.epsilon1 >= thresholds.epsilon1_degrade ||
      metrics.epsilon2 >= thresholds.epsilon2_degrade ||
      metrics.epsilon3 >= thresholds.epsilon3_degrade;
  if (degrade) return CurveQualityAction::kDegrade;
  return CurveQualityAction::kAccept;
}

cex::common::Decimal DoubleToDecimalWithScale(const double value, const int32_t scale) {
  const double factor = std::pow(10.0, static_cast<double>(scale));
  const double scaled = std::round(value * factor);
  if (scaled > static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return cex::common::Decimal{std::numeric_limits<int64_t>::max(), scale};
  }
  if (scaled < static_cast<double>(std::numeric_limits<int64_t>::min())) {
    return cex::common::Decimal{std::numeric_limits<int64_t>::min(), scale};
  }
  return cex::common::Decimal{static_cast<int64_t>(scaled), scale};
}

int32_t FitScaleToMagnitude(const double max_abs_value, int32_t requested_scale) {
  int32_t scale = std::max(0, requested_scale);
  if (!std::isfinite(max_abs_value) || max_abs_value <= 0.0) {
    return scale;
  }

  while (scale > 0) {
    const long double scaled = static_cast<long double>(max_abs_value) *
                               std::pow(10.0L, static_cast<long double>(scale));
    if (scaled <= static_cast<long double>(std::numeric_limits<int64_t>::max())) {
      break;
    }
    --scale;
  }
  return scale;
}

double SnapshotTakerFeeRate(const fob::venue::v1::VenueSnapshot& snapshot) {
  if (!snapshot.has_taker_fee()) return 0.0;
  return ClampFinite(ProtoDecimalAsDouble(snapshot.taker_fee()), 0.0, 0.999999);
}

std::vector<domain::BookLevel> ApplyTakerFeeToSide(
    const std::vector<domain::BookLevel>& levels,
    const domain::BookSide side,
    const double taker_fee,
    const int32_t min_price_scale) {
  if (levels.empty() || taker_fee <= 0.0) return levels;

  const double multiplier = (side == domain::BookSide::kAsk)
      ? (1.0 + taker_fee)
      : std::max(0.0, 1.0 - taker_fee);
  std::vector<domain::BookLevel> out;
  out.reserve(levels.size());
  for (const auto& level : levels) {
    const double adjusted_price =
        DecimalAsDouble(level.price) * multiplier;
    if (!std::isfinite(adjusted_price) || adjusted_price <= 0.0) continue;
    const int32_t price_scale = std::max(level.price.scale, min_price_scale);
    out.push_back(domain::BookLevel{
        .price = DoubleToDecimalWithScale(adjusted_price, price_scale),
        .qty = level.qty,
    });
  }
  return out;
}

domain::CanonicalOrderBook ApplyTakerFeeToBook(
    domain::CanonicalOrderBook book,
    const double taker_fee,
    const LiquidityCurveInputConfig& input) {
  if (!input.apply_taker_fee || taker_fee <= 0.0) return book;
  book.bids = ApplyTakerFeeToSide(
      book.bids, domain::BookSide::kBid, taker_fee,
      input.fee_adjusted_price_scale);
  book.asks = ApplyTakerFeeToSide(
      book.asks, domain::BookSide::kAsk, taker_fee,
      input.fee_adjusted_price_scale);
  return book;
}

std::string BoolTag(const bool value) {
  return value ? "true" : "false";
}

struct SyntheticCurveStats {
  double p_low{0.0};
  double p_high{0.0};
  double q_max{0.0};
};

std::optional<SyntheticCurveStats> ExtractSyntheticCurveStats(
    const fob::venue::v1::SideLiquidityCurve& curve) {
  if (curve.q_grid_size() == 0 || curve.p_of_q_size() == 0) {
    return std::nullopt;
  }

  double q_max = 0.0;
  for (const double q : curve.q_grid()) {
    if (std::isfinite(q) && q > q_max) {
      q_max = q;
    }
  }
  if (q_max <= 0.0) {
    return std::nullopt;
  }

  double p_low = std::numeric_limits<double>::infinity();
  double p_high = -std::numeric_limits<double>::infinity();
  for (const double price : curve.p_of_q()) {
    if (!std::isfinite(price) || price <= 0.0) continue;
    p_low = std::min(p_low, price);
    p_high = std::max(p_high, price);
  }
  if (!std::isfinite(p_low) || !std::isfinite(p_high) || p_high <= 0.0) {
    return std::nullopt;
  }

  return SyntheticCurveStats{
      .p_low = p_low,
      .p_high = p_high,
      .q_max = q_max,
  };
}

const char* SideText(const fob::common::v1::Side side) {
  switch (side) {
    case fob::common::v1::SIDE_BUY:
      return "buy";
    case fob::common::v1::SIDE_SELL:
      return "sell";
    default:
      return "unspecified";
  }
}

}  // namespace

LiquidityCurveProducer::LiquidityCurveProducer(
    IMessagePublisher* publisher,
    LiquidityCurveProducerConfig config)
    : LiquidityCurveProducer(publisher, nullptr, std::move(config)) {}

LiquidityCurveProducer::LiquidityCurveProducer(
    IMessagePublisher* publisher,
    ISyntheticOrderRepository* synthetic_order_repository,
    LiquidityCurveProducerConfig config)
    : publisher_(publisher),
      synthetic_order_repository_(synthetic_order_repository),
      config_(std::move(config)) {
  config_.quality_thresholds = NormalizeQualityThresholds(config_.quality_thresholds);
  config_.synthetic = NormalizeSyntheticConfig(config_.synthetic);
  config_.input = NormalizeInputConfig(config_.input);
  if (config_.liquidity_fob_versioning.model_config_version.empty()) {
    config_.liquidity_fob_versioning.model_config_version = "default";
  }
}

bool LiquidityCurveProducer::Publish(
    const fob::venue::v1::VenueSnapshot& snapshot,
    fob::venue::v1::VenueLiquidityCurve* published_curve,
    std::vector<fob::orders::v1::SyntheticFlowOrder>* published_synthetics,
    const PublishOptions& options) {
  const auto build_started_at = std::chrono::steady_clock::now();
  if (published_curve != nullptr) {
    published_curve->Clear();
  }
  if (published_synthetics != nullptr) {
    published_synthetics->clear();
  }
  if (publisher_ == nullptr) return false;
  if (snapshot.venue_id().empty() || snapshot.instrument().symbol().empty()) return false;

  const std::string requested_level_text = options.requested_level.has_value() &&
          !options.requested_level->empty()
      ? *options.requested_level
      : config_.level;
  const CurveBuildLevel requested_level = ParseLevel(requested_level_text);
  if (requested_level == CurveBuildLevel::kOff) return false;
  SyntheticFlowOrderConfig synthetic_config = config_.synthetic;
  if (options.synthetic_enabled.has_value()) {
    synthetic_config.enabled = *options.synthetic_enabled;
  }

  const std::string symbol = snapshot.instrument().symbol();
  const std::string symbol_key = SymbolKey(snapshot.venue_id(), symbol);

  DegradationState degradation_state;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    const auto it = degradation_state_by_symbol_.find(symbol_key);
    if (it != degradation_state_by_symbol_.end()) {
      degradation_state = it->second;
    }
  }

  CurveBuildLevel max_level = requested_level;
  double signal_multiplier = 1.0;
  std::string degrade_reason = "ok";
  const bool suppress_synthetic = IsStaleStatus(snapshot.status());

  if (IsOffStatus(snapshot.status())) {
    cex::common::log_json("WARN", "Venue curve builder degraded to OFF",
                          {{"service", "venues"},
                           {"component", "venue_liquidity_curve_builder"},
                           {"participant", "Venue Liquidity Curve Builder"},
                           {"stage", "disable_curve_build"},
                           {"venue", snapshot.venue_id()},
                           {"symbol", symbol},
                           {"requested_level", LevelToString(requested_level)},
                           {"status", snapshot.status()},
                           {"reason", "snapshot_off_status"},
                           {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
    return false;
  }

  if (suppress_synthetic && !config_.degradation.publish_stale_l1_fallback) {
    cex::common::log_json("WARN", "Skipped venue.liquidity.fob for stale snapshot",
                          {{"service", "venues"},
                           {"component", "venue_liquidity_curve_builder"},
                           {"participant", "Venue Liquidity Curve Builder"},
                           {"stage", "skip_curve_build"},
                           {"topic", config_.topic},
                           {"venue", snapshot.venue_id()},
                           {"symbol", symbol},
                           {"requested_level", LevelToString(requested_level)},
                           {"status", snapshot.status()},
                           {"reason", "stale_snapshot_and_fallback_disabled"},
                           {"publish_stale_l1_fallback",
                            BoolTag(config_.degradation.publish_stale_l1_fallback)},
                           {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
    return false;
  }

  if (requested_level == CurveBuildLevel::kL3 && !config_.l3_impact.enabled) {
    max_level = CurveBuildLevel::kL2;
    degrade_reason = "l3_disabled";
  }

  if (config_.degradation.enabled) {
    if (IsStaleStatus(snapshot.status())) {
      max_level = MinLevel(max_level, CurveBuildLevel::kL1);
      signal_multiplier *= ClampFinite(
          config_.degradation.stale_confidence_multiplier, 0.0, 1.0);
      degrade_reason = "stale";
    }

    const uint32_t operational_errors =
        degradation_state.consecutive_publish_errors +
        degradation_state.consecutive_execution_errors;
    if (operational_errors >= config_.degradation.max_consecutive_errors_off) {
      cex::common::log_json("WARN", "Venue curve builder degraded to OFF",
                            {{"service", "venues"},
                             {"component", "venue_liquidity_curve_builder"},
                             {"participant", "Venue Liquidity Curve Builder"},
                             {"stage", "disable_curve_build"},
                             {"venue", snapshot.venue_id()},
                             {"symbol", symbol},
                             {"requested_level", LevelToString(requested_level)},
                             {"reason", "too_many_errors"},
                             {"errors", std::to_string(operational_errors)},
                             {"max_consecutive_errors_off",
                              std::to_string(config_.degradation.max_consecutive_errors_off)},
                             {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
      return false;
    }
    if (operational_errors > 0) {
      max_level = DowngradeBySteps(max_level, operational_errors);
      const double error_penalty = ClampFinite(
          config_.degradation.error_confidence_penalty, 0.0, 1.0);
      signal_multiplier *= std::max(0.0, 1.0 - error_penalty * operational_errors);
      degrade_reason = "errors";
    }
  }

  if (max_level == CurveBuildLevel::kOff) {
    cex::common::log_json("WARN", "Venue curve builder degraded to OFF",
                          {{"service", "venues"},
                           {"component", "venue_liquidity_curve_builder"},
                           {"participant", "Venue Liquidity Curve Builder"},
                           {"stage", "disable_curve_build"},
                           {"venue", snapshot.venue_id()},
                           {"symbol", symbol},
                           {"requested_level", LevelToString(requested_level)},
                           {"reason", degrade_reason},
                           {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
    return false;
  }

  const std::vector<domain::BookLevel> bids = ToBookLevels(
      snapshot.bid_prices(), snapshot.bid_quantities());
  const std::vector<domain::BookLevel> asks = ToBookLevels(
      snapshot.ask_prices(), snapshot.ask_quantities());
  const double taker_fee_rate = SnapshotTakerFeeRate(snapshot);
  const cex::common::Decimal tau_sec = TauSecondsFromMs(config_.tau_ms);

  domain::DepthSideCurves bid_virtual_base;
  domain::DepthSideCurves ask_virtual_base;
  bool has_virtual_base = false;

  if (!bids.empty() && !asks.empty()) {
    domain::DepthCanonicalizationConfig canonical_cfg;
    if (snapshot.has_tick_size()) {
      canonical_cfg.tick_size = cex::common::Decimal::from_proto(snapshot.tick_size());
    }
    if (snapshot.has_lot_size()) {
      canonical_cfg.lot_size = cex::common::Decimal::from_proto(snapshot.lot_size());
    }
    canonical_cfg.min_qty = config_.input.min_qty;
    canonical_cfg.max_levels_per_side = 0;

    domain::CanonicalOrderBook book =
        domain::CanonicalizeOrderBook(bids, asks, canonical_cfg);
    book = ApplyTakerFeeToBook(book, taker_fee_rate, config_.input);
    if (!book.is_empty()) {
      bid_virtual_base =
          domain::BuildDepthSideCurves(book, domain::ExecutionSide::kSell, tau_sec);
      ask_virtual_base =
          domain::BuildDepthSideCurves(book, domain::ExecutionSide::kBuy, tau_sec);
      has_virtual_base = !bid_virtual_base.empty() && !ask_virtual_base.empty();
    }
  }

  domain::DepthSideCurves bid_base = bid_virtual_base;
  domain::DepthSideCurves ask_base = ask_virtual_base;
  bool using_direct_amm = false;
  double direct_vs_virtual_price_bps = 0.0;
  double direct_vs_virtual_cost_rel = 0.0;
  double amm_pool_fee_rate_applied = taker_fee_rate;
  if (std::isfinite(config_.amm_direct.pool_fee_rate_override) &&
      config_.amm_direct.pool_fee_rate_override >= 0.0) {
    amm_pool_fee_rate_applied =
        std::clamp(std::abs(config_.amm_direct.pool_fee_rate_override), 0.0, 0.999999);
  } else {
    amm_pool_fee_rate_applied = std::clamp(std::abs(amm_pool_fee_rate_applied), 0.0, 0.999999);
  }
  const double amm_gas_cost_quote_applied = std::max(
      0.0, std::isfinite(config_.amm_direct.gas_cost_quote)
               ? config_.amm_direct.gas_cost_quote
               : 0.0);
  const double amm_execution_overhead_bps_applied = std::max(
      0.0, std::isfinite(config_.amm_direct.execution_overhead_bps)
               ? config_.amm_direct.execution_overhead_bps
               : 0.0);
  const double amm_slippage_multiplier_applied =
      (std::isfinite(config_.amm_direct.slippage_multiplier) &&
       config_.amm_direct.slippage_multiplier > 0.0)
      ? config_.amm_direct.slippage_multiplier
      : 1.0;
  const double amm_effective_fee_rate_applied = std::clamp(
      amm_pool_fee_rate_applied + amm_execution_overhead_bps_applied / 10000.0,
      0.0, 0.999999);

  if (config_.amm_direct.enabled) {
    const std::optional<domain::VenuePoolState> pool_state =
        TryExtractAmmPoolState(snapshot);
    if (pool_state.has_value()) {
      domain::DirectAmmFobConfig direct_cfg;
      direct_cfg.max_segments_per_side =
          std::max<std::size_t>(1, config_.amm_direct.max_segments_per_side);
      direct_cfg.price_scale =
          snapshot.has_best_bid() ? snapshot.best_bid().scale() : 8;
      if (direct_cfg.price_scale < 0) direct_cfg.price_scale = 8;
      direct_cfg.qty_scale =
          snapshot.bid_quantities_size() > 0 ? snapshot.bid_quantities(0).scale() : 8;
      if (direct_cfg.qty_scale < 0) direct_cfg.qty_scale = 8;
      direct_cfg.pool_fee_rate = amm_pool_fee_rate_applied;
      direct_cfg.gas_cost_quote = amm_gas_cost_quote_applied;
      direct_cfg.execution_overhead_bps = amm_execution_overhead_bps_applied;
      direct_cfg.slippage_multiplier = amm_slippage_multiplier_applied;

      domain::DepthSideCurves bid_direct = domain::BuildDirectAmmFobCurves(
          *pool_state, domain::ExecutionSide::kSell, tau_sec, direct_cfg);
      domain::DepthSideCurves ask_direct = domain::BuildDirectAmmFobCurves(
          *pool_state, domain::ExecutionSide::kBuy, tau_sec, direct_cfg);
      if (!bid_direct.empty() && !ask_direct.empty()) {
        using_direct_amm = true;
        bid_base = std::move(bid_direct);
        ask_base = std::move(ask_direct);

        if (config_.amm_direct.compare_with_virtual_lob && has_virtual_base) {
          direct_vs_virtual_price_bps =
              0.5 * (ComputePriceErrorBps(bid_virtual_base.p_of_q, bid_base.p_of_q) +
                     ComputePriceErrorBps(ask_virtual_base.p_of_q, ask_base.p_of_q));
          direct_vs_virtual_cost_rel =
              0.5 * (ComputeRelativeCostError(bid_virtual_base.s_of_q, bid_base.s_of_q) +
                     ComputeRelativeCostError(ask_virtual_base.s_of_q, ask_base.s_of_q));
        }
      }
    }
  }

  if (bid_base.empty() || ask_base.empty()) return false;

  UpdateReferenceMid(snapshot);

  const auto build_level = [&](const domain::DepthSideCurves& base,
                               const CurveBuildLevel level,
                               const domain::ExecutionSide side) {
    domain::DepthSideCurves curves = base;
    if (level == CurveBuildLevel::kL1) {
      if (config_.apply_fenchel_legendre) {
        curves = domain::ApplyFenchelLegendreLayer(curves, config_.fenchel_legendre);
      }
      return curves;
    }
    curves = ApplyL2Pipeline(curves, config_);
    if (level == CurveBuildLevel::kL3 && config_.l3_impact.enabled) {
      curves = ApplyL3ImpactCalibration(curves, snapshot.venue_id(), symbol, side);
    }
    return curves;
  };

  const auto evaluate_quality = [&](const CurveBuildLevel level,
                                    const domain::DepthSideCurves& bid_curve,
                                    const domain::DepthSideCurves& ask_curve) {
    CurveQuality quality;
    quality.epsilon1 =
        0.5 * (ComputePriceErrorBps(bid_base.p_of_q, bid_curve.p_of_q) +
               ComputePriceErrorBps(ask_base.p_of_q, ask_curve.p_of_q));
    quality.epsilon2 =
        0.5 * (ComputeShapeErrorBps(
                   bid_curve.p_of_q, bid_curve.s_of_q, domain::ExecutionSide::kSell) +
               ComputeShapeErrorBps(
                   ask_curve.p_of_q, ask_curve.s_of_q, domain::ExecutionSide::kBuy));
    if (level == CurveBuildLevel::kL3 && config_.l3_impact.enabled) {
      quality.epsilon3 =
          0.5 * (ComputeExecutionCalibrationErrorBps(
                    bid_curve, snapshot.venue_id(), symbol, domain::ExecutionSide::kSell) +
                 ComputeExecutionCalibrationErrorBps(
                    ask_curve, snapshot.venue_id(), symbol, domain::ExecutionSide::kBuy));
    }

    const double eps1_scale = SafePositiveScale(config_.degradation.epsilon1_green_bps, 1.0);
    const double eps2_scale = SafePositiveScale(config_.degradation.epsilon2_green_bps, 0.5);
    const double eps3_scale = SafePositiveScale(config_.degradation.epsilon3_green_bps, 2.0);
    const double epsilon_pressure =
        quality.epsilon1 / eps1_scale +
        quality.epsilon2 / eps2_scale +
        quality.epsilon3 / eps3_scale;

    quality.confidence = Clamp01(1.0 / (1.0 + epsilon_pressure));
    if (level == CurveBuildLevel::kL3 && config_.l3_impact.enabled) {
      const bool has_sell_samples = HasExecutionSamples(
          snapshot.venue_id(), symbol, domain::ExecutionSide::kSell);
      const bool has_buy_samples = HasExecutionSamples(
          snapshot.venue_id(), symbol, domain::ExecutionSide::kBuy);
      const double coverage = 0.5 * (static_cast<double>(has_sell_samples) +
                                     static_cast<double>(has_buy_samples));
      const double no_history_multiplier = ClampFinite(
          config_.degradation.l3_no_execution_confidence_multiplier, 0.0, 1.0);
      quality.confidence *= coverage + (1.0 - coverage) * no_history_multiplier;
    }
    quality.confidence = Clamp01(quality.confidence * signal_multiplier);
    return quality;
  };

  const auto evaluate_legacy_quality = [&](const CurveBuildLevel level,
                                           const domain::DepthSideCurves& bid_curve,
                                           const domain::DepthSideCurves& ask_curve) {
    LegacyQualityMetrics metrics;
    metrics.epsilon1 =
        0.5 * (ComputeRelativeCostError(bid_base.s_of_q, bid_curve.s_of_q) +
               ComputeRelativeCostError(ask_base.s_of_q, ask_curve.s_of_q));

    const double bid_epsilon2 = std::max(
        ComputeMonotonicityError(bid_curve.p_of_q, domain::ExecutionSide::kSell),
        ComputeConvexityError(bid_curve.s_of_q, domain::ExecutionSide::kSell));
    const double ask_epsilon2 = std::max(
        ComputeMonotonicityError(ask_curve.p_of_q, domain::ExecutionSide::kBuy),
        ComputeConvexityError(ask_curve.s_of_q, domain::ExecutionSide::kBuy));
    metrics.epsilon2 = 0.5 * (bid_epsilon2 + ask_epsilon2);

    metrics.epsilon3 = 0.5 * (ComputeDualPenalty(bid_curve) + ComputeDualPenalty(ask_curve));
    if (level == CurveBuildLevel::kL3 && config_.l3_impact.enabled) {
      const double exec_error =
          0.5 * (ComputeExecutionCalibrationError(
                     snapshot.venue_id(), symbol, domain::ExecutionSide::kSell) +
                 ComputeExecutionCalibrationError(
                     snapshot.venue_id(), symbol, domain::ExecutionSide::kBuy));
      metrics.epsilon3 = 0.5 * (metrics.epsilon3 + exec_error);
    }

    metrics.confidence = Clamp01(
        1.0 / (1.0 + metrics.epsilon1 + metrics.epsilon2 + metrics.epsilon3));
    return metrics;
  };

  domain::DepthSideCurves bid_curve;
  domain::DepthSideCurves ask_curve;
  CurveQuality selected_quality;
  CurveBuildLevel effective_level = CurveBuildLevel::kOff;
  CurveQualityAction selected_quality_action = CurveQualityAction::kAccept;
  bool hard_disabled = false;

  const std::array<CurveBuildLevel, 3> levels{
      CurveBuildLevel::kL3,
      CurveBuildLevel::kL2,
      CurveBuildLevel::kL1,
  };
  for (const CurveBuildLevel candidate_level : levels) {
    if (static_cast<int>(candidate_level) > static_cast<int>(max_level)) continue;

    domain::DepthSideCurves candidate_bid = build_level(
        bid_base, candidate_level, domain::ExecutionSide::kSell);
    domain::DepthSideCurves candidate_ask = build_level(
        ask_base, candidate_level, domain::ExecutionSide::kBuy);
    if (candidate_bid.empty() || candidate_ask.empty()) {
      cex::common::log_json("INFO", "Venue curve candidate rejected",
                            {{"service", "venues"},
                             {"component", "venue_liquidity_curve_builder"},
                             {"participant", "Venue Liquidity Curve Builder"},
                             {"stage", "evaluate_curve_candidate"},
                             {"venue", snapshot.venue_id()},
                             {"symbol", symbol},
                             {"requested_level", LevelToString(requested_level)},
                             {"max_level", LevelToString(max_level)},
                             {"candidate_level", LevelToString(candidate_level)},
                             {"reason", "empty_curve"},
                             {"bid_empty", candidate_bid.empty() ? "true" : "false"},
                             {"ask_empty", candidate_ask.empty() ? "true" : "false"},
                             {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
      degrade_reason = "empty_curve";
      continue;
    }

    CurveQuality candidate_quality = evaluate_quality(
        candidate_level, candidate_bid, candidate_ask);
    const bool has_lower_level =
        static_cast<int>(candidate_level) > static_cast<int>(CurveBuildLevel::kL1);
    const CurveQualityAction legacy_action = DecideQualityAction(
        evaluate_legacy_quality(candidate_level, candidate_bid, candidate_ask),
        config_.quality_thresholds,
        config_.enable_quality_gating);
    if (legacy_action == CurveQualityAction::kDisable) {
      hard_disabled = true;
      degrade_reason = "quality_gating_disable";
      cex::common::log_json("INFO", "Venue curve candidate rejected",
                            {{"service", "venues"},
                             {"component", "venue_liquidity_curve_builder"},
                             {"participant", "Venue Liquidity Curve Builder"},
                             {"stage", "evaluate_curve_candidate"},
                             {"venue", snapshot.venue_id()},
                             {"symbol", symbol},
                             {"requested_level", LevelToString(requested_level)},
                             {"max_level", LevelToString(max_level)},
                             {"candidate_level", LevelToString(candidate_level)},
                             {"reason", degrade_reason},
                             {"quality_action", "disable"},
                             {"confidence", std::to_string(candidate_quality.confidence)},
                             {"epsilon1", std::to_string(candidate_quality.epsilon1)},
                             {"epsilon2", std::to_string(candidate_quality.epsilon2)},
                             {"epsilon3", std::to_string(candidate_quality.epsilon3)},
                             {"epsilon1_degrade",
                              std::to_string(config_.quality_thresholds.epsilon1_degrade)},
                             {"epsilon1_disable",
                              std::to_string(config_.quality_thresholds.epsilon1_disable)},
                             {"epsilon2_degrade",
                              std::to_string(config_.quality_thresholds.epsilon2_degrade)},
                             {"epsilon2_disable",
                              std::to_string(config_.quality_thresholds.epsilon2_disable)},
                             {"epsilon3_degrade",
                              std::to_string(config_.quality_thresholds.epsilon3_degrade)},
                             {"epsilon3_disable",
                              std::to_string(config_.quality_thresholds.epsilon3_disable)},
                             {"bid_q_points", std::to_string(candidate_bid.p_of_q.size())},
                             {"ask_q_points", std::to_string(candidate_ask.p_of_q.size())},
                             {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
      continue;
    }
    if (legacy_action == CurveQualityAction::kDegrade && has_lower_level) {
      degrade_reason = "quality_gating";
      cex::common::log_json("INFO", "Venue curve candidate rejected",
                            {{"service", "venues"},
                             {"component", "venue_liquidity_curve_builder"},
                             {"participant", "Venue Liquidity Curve Builder"},
                             {"stage", "evaluate_curve_candidate"},
                             {"venue", snapshot.venue_id()},
                             {"symbol", symbol},
                             {"requested_level", LevelToString(requested_level)},
                             {"max_level", LevelToString(max_level)},
                             {"candidate_level", LevelToString(candidate_level)},
                             {"reason", degrade_reason},
                             {"quality_action", "degrade"},
                             {"confidence", std::to_string(candidate_quality.confidence)},
                             {"epsilon1", std::to_string(candidate_quality.epsilon1)},
                             {"epsilon2", std::to_string(candidate_quality.epsilon2)},
                             {"epsilon3", std::to_string(candidate_quality.epsilon3)},
                             {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
      continue;
    }

    const double min_confidence = MinConfidenceForLevel(candidate_level, config_.degradation);
    if (!config_.degradation.enabled || candidate_quality.confidence >= min_confidence) {
      effective_level = candidate_level;
      bid_curve = std::move(candidate_bid);
      ask_curve = std::move(candidate_ask);
      selected_quality = candidate_quality;
      selected_quality_action = legacy_action;
      cex::common::log_json("INFO", "Venue curve candidate accepted",
                            {{"service", "venues"},
                             {"component", "venue_liquidity_curve_builder"},
                             {"participant", "Venue Liquidity Curve Builder"},
                             {"stage", "evaluate_curve_candidate"},
                             {"venue", snapshot.venue_id()},
                             {"symbol", symbol},
                             {"requested_level", LevelToString(requested_level)},
                             {"max_level", LevelToString(max_level)},
                             {"candidate_level", LevelToString(candidate_level)},
                             {"quality_action",
                              QualityActionToString(legacy_action,
                                                    config_.enable_quality_gating)},
                             {"confidence", std::to_string(candidate_quality.confidence)},
                             {"min_confidence", std::to_string(min_confidence)},
                             {"epsilon1", std::to_string(candidate_quality.epsilon1)},
                             {"epsilon2", std::to_string(candidate_quality.epsilon2)},
                             {"epsilon3", std::to_string(candidate_quality.epsilon3)},
                             {"bid_q_points", std::to_string(candidate_bid.p_of_q.size())},
                             {"ask_q_points", std::to_string(candidate_ask.p_of_q.size())},
                             {"bid_q_max", std::to_string(CurveQMax(candidate_bid))},
                             {"ask_q_max", std::to_string(CurveQMax(candidate_ask))},
                             {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
      break;
    }
    degrade_reason = "curve_quality";
    cex::common::log_json("INFO", "Venue curve candidate rejected",
                          {{"service", "venues"},
                           {"component", "venue_liquidity_curve_builder"},
                           {"participant", "Venue Liquidity Curve Builder"},
                           {"stage", "evaluate_curve_candidate"},
                           {"venue", snapshot.venue_id()},
                           {"symbol", symbol},
                           {"requested_level", LevelToString(requested_level)},
                           {"max_level", LevelToString(max_level)},
                           {"candidate_level", LevelToString(candidate_level)},
                           {"reason", degrade_reason},
                           {"confidence", std::to_string(candidate_quality.confidence)},
                           {"min_confidence", std::to_string(min_confidence)},
                           {"epsilon1", std::to_string(candidate_quality.epsilon1)},
                           {"epsilon2", std::to_string(candidate_quality.epsilon2)},
                           {"epsilon3", std::to_string(candidate_quality.epsilon3)},
                           {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
  }

  if (effective_level == CurveBuildLevel::kOff && !hard_disabled) {
    if (static_cast<int>(max_level) >= static_cast<int>(CurveBuildLevel::kL1)) {
      domain::DepthSideCurves fallback_bid = build_level(
          bid_base, CurveBuildLevel::kL1, domain::ExecutionSide::kSell);
      domain::DepthSideCurves fallback_ask = build_level(
          ask_base, CurveBuildLevel::kL1, domain::ExecutionSide::kBuy);
      if (!fallback_bid.empty() && !fallback_ask.empty()) {
        bid_curve = std::move(fallback_bid);
        ask_curve = std::move(fallback_ask);
        selected_quality = evaluate_quality(
            CurveBuildLevel::kL1, bid_curve, ask_curve);
        selected_quality_action = CurveQualityAction::kDegrade;
        effective_level = CurveBuildLevel::kL1;
        degrade_reason = "forced_l1_fallback";
        cex::common::log_json("WARN", "Venue curve builder forced L1 fallback",
                              {{"service", "venues"},
                               {"component", "venue_liquidity_curve_builder"},
                               {"participant", "Venue Liquidity Curve Builder"},
                               {"stage", "force_l1_fallback"},
                               {"venue", snapshot.venue_id()},
                               {"symbol", symbol},
                               {"requested_level", LevelToString(requested_level)},
                               {"max_level", LevelToString(max_level)},
                               {"reason", degrade_reason},
                               {"confidence", std::to_string(selected_quality.confidence)},
                               {"epsilon1", std::to_string(selected_quality.epsilon1)},
                               {"epsilon2", std::to_string(selected_quality.epsilon2)},
                               {"epsilon3", std::to_string(selected_quality.epsilon3)},
                               {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
      }
    }
  }

  if (effective_level == CurveBuildLevel::kOff) {
    cex::common::log_json("WARN", "Venue curve builder degraded to OFF",
                          {{"service", "venues"},
                           {"component", "venue_liquidity_curve_builder"},
                           {"participant", "Venue Liquidity Curve Builder"},
                           {"stage", "disable_curve_build"},
                           {"venue", snapshot.venue_id()},
                           {"symbol", symbol},
                           {"requested_level", LevelToString(requested_level)},
                           {"max_level", LevelToString(max_level)},
                           {"reason", degrade_reason},
                           {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
    return false;
  }

  fob::venue::v1::VenueLiquidityCurve curve;
  if (snapshot.has_meta()) {
    *curve.mutable_meta() = snapshot.meta();
  }
  auto* meta = curve.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("venues");
  if (meta->correlation_id().empty()) {
    if (snapshot.has_meta() && !snapshot.meta().event_id().empty()) {
      meta->set_correlation_id(snapshot.meta().event_id());
    } else {
      meta->set_correlation_id(cex::common::uuid_v4());
    }
  }
  meta->set_partition_key(CurvePartitionKey(snapshot));

  curve.set_venue_id(snapshot.venue_id());
  *curve.mutable_instrument() = snapshot.instrument();
  if (snapshot.has_timestamp()) {
    *curve.mutable_timestamp() = snapshot.timestamp();
  }
  if (snapshot.has_meta()) {
    curve.set_snapshot_id(snapshot.meta().event_id());
  }
  curve.set_curve_id(meta->event_id());

  *curve.mutable_bid_curve() = ToProtoCurve(bid_curve);
  *curve.mutable_ask_curve() = ToProtoCurve(ask_curve);

  curve.set_epsilon1(selected_quality.epsilon1);
  curve.set_epsilon2(selected_quality.epsilon2);
  curve.set_epsilon3(selected_quality.epsilon3);
  curve.set_confidence(selected_quality.confidence);
  curve.set_level(LevelToString(effective_level));
  if (snapshot.has_mid_price()) {
    *curve.mutable_mid_price() = snapshot.mid_price();
  }
  const EffectiveTauMetrics tau_metrics = ComputeEffectiveTauMetrics(
      snapshot, bid_curve, ask_curve, config_.tau_ms);
  curve.set_tau_ms(tau_metrics.effective_tau_ms);
  ApplyLiquidityFobVersioning(&curve);

  const auto build_finished_at = std::chrono::steady_clock::now();
  const double build_latency_ms =
      static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                              build_finished_at - build_started_at)
                              .count()) /
      1000.0;
  auto* tags = curve.mutable_meta()->mutable_tags();
  (*tags)["model_config_version"] =
      config_.liquidity_fob_versioning.model_config_version;
  (*tags)["requested_level"] = LevelToString(requested_level);
  (*tags)["effective_level"] = LevelToString(effective_level);
  (*tags)["degradation_reason"] = degrade_reason;
  (*tags)["quality_action"] = QualityActionToString(
      selected_quality_action, config_.enable_quality_gating);
  (*tags)["confidence"] = std::to_string(curve.confidence());
  (*tags)["epsilon1_bps"] = std::to_string(curve.epsilon1());
  (*tags)["epsilon2_bps"] = std::to_string(curve.epsilon2());
  (*tags)["epsilon3_bps"] = std::to_string(curve.epsilon3());
  (*tags)["tau_ms"] = std::to_string(curve.tau_ms());
  (*tags)["base_tau_ms"] = std::to_string(tau_metrics.base_tau_ms);
  (*tags)["q_max"] = std::to_string(tau_metrics.q_max);
  (*tags)["volume_24h_qty"] = std::to_string(tau_metrics.volume_24h_qty);
  (*tags)["raw_hourly_rate"] = std::to_string(tau_metrics.raw_hourly_rate);
  (*tags)["hourly_turnover_cap"] = std::to_string(tau_metrics.hourly_turnover_cap);
  (*tags)["tau_adjustment_reason"] =
      tau_metrics.capped_by_turnover ? "volume_24h_turnover_cap" : "config";
  (*tags)["build_latency_ms"] = std::to_string(build_latency_ms);
  (*tags)["apply_taker_fee"] = BoolTag(config_.input.apply_taker_fee);
  (*tags)["taker_fee"] = std::to_string(taker_fee_rate);
  (*tags)["min_qty"] = config_.input.min_qty.to_string();
  (*tags)["l2_convexification"] = BoolTag(config_.apply_convexification);
  (*tags)["l2_moreau"] = BoolTag(config_.apply_moreau_l2);
  (*tags)["l2_tikhonov"] = BoolTag(config_.apply_tikhonov_l2);
  (*tags)["dual_enabled"] = BoolTag(config_.apply_fenchel_legendre);
  (*tags)["l3_impact_enabled"] = BoolTag(config_.l3_impact.enabled);
  (*tags)["l3_impact_model"] = ImpactModelToString(config_.l3_impact.model);
  (*tags)["l3_execution_blend_weight"] =
      std::to_string(config_.l3_impact.execution_blend_weight);
  (*tags)["amm_path"] = using_direct_amm ? "direct" : "virtual_lob";
  (*tags)["amm_direct_enabled"] = BoolTag(config_.amm_direct.enabled);
  (*tags)["amm_pool_fee_rate"] = std::to_string(amm_pool_fee_rate_applied);
  (*tags)["amm_gas_cost_quote"] = std::to_string(amm_gas_cost_quote_applied);
  (*tags)["amm_execution_overhead_bps"] = std::to_string(amm_execution_overhead_bps_applied);
  (*tags)["amm_slippage_multiplier"] = std::to_string(amm_slippage_multiplier_applied);
  (*tags)["amm_effective_fee_rate"] = std::to_string(amm_effective_fee_rate_applied);
  (*tags)["calibration.amm.pool_fee_rate"] = std::to_string(amm_pool_fee_rate_applied);
  (*tags)["calibration.amm.gas_cost_quote"] = std::to_string(amm_gas_cost_quote_applied);
  (*tags)["calibration.amm.execution_overhead_bps"] =
      std::to_string(amm_execution_overhead_bps_applied);
  (*tags)["calibration.amm.slippage_multiplier"] =
      std::to_string(amm_slippage_multiplier_applied);
  (*tags)["calibration.amm.effective_fee_rate"] =
      std::to_string(amm_effective_fee_rate_applied);
  if (using_direct_amm && config_.amm_direct.compare_with_virtual_lob && has_virtual_base) {
    (*tags)["amm_direct_vs_virtual_price_bps"] =
        std::to_string(std::max(0.0, direct_vs_virtual_price_bps));
    (*tags)["amm_direct_vs_virtual_cost_rel"] =
        std::to_string(std::max(0.0, direct_vs_virtual_cost_rel));
  }

  const std::string key = meta->partition_key().empty()
      ? (curve.venue_id() + "|" + curve.instrument().symbol())
      : meta->partition_key();

  const bool ok = PublishLiquidityFob(curve, key);
  if (tau_metrics.capped_by_turnover) {
    cex::common::log_json("INFO", "Adjusted venue tau by 24h turnover cap",
                          {{"service", "venues"},
                           {"component", "venue_liquidity_curve_builder"},
                           {"participant", "Venue Liquidity Curve Builder"},
                           {"stage", "adjust_tau"},
                           {"venue", curve.venue_id()},
                           {"symbol", curve.instrument().symbol()},
                           {"base_tau_ms", std::to_string(tau_metrics.base_tau_ms)},
                           {"effective_tau_ms", std::to_string(tau_metrics.effective_tau_ms)},
                           {"q_max", std::to_string(tau_metrics.q_max)},
                           {"volume_24h_qty", std::to_string(tau_metrics.volume_24h_qty)},
                           {"raw_hourly_rate", std::to_string(tau_metrics.raw_hourly_rate)},
                           {"hourly_turnover_cap",
                            std::to_string(tau_metrics.hourly_turnover_cap)},
                           {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
  }
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    auto& state = degradation_state_by_symbol_[symbol_key];
    if (ok) {
      state.consecutive_publish_errors = 0;
    } else {
      ++state.consecutive_publish_errors;
    }
  }
  cex::common::log_json(ok ? "INFO" : "ERROR", "Published venue.liquidity.fob",
                        {{"service", "venues"},
                         {"component", "venue_liquidity_curve_builder"},
                         {"participant", "Venue Liquidity Curve Builder"},
                         {"stage", "publish_curve"},
                         {"topic", config_.topic},
                         {"venue", curve.venue_id()},
                         {"symbol", curve.instrument().symbol()},
                         {"requested_level", LevelToString(requested_level)},
                         {"level", curve.level()},
                         {"quality_action",
                          QualityActionToString(selected_quality_action,
                                                config_.enable_quality_gating)},
                         {"confidence", std::to_string(curve.confidence())},
                         {"epsilon1", std::to_string(curve.epsilon1())},
                         {"epsilon2", std::to_string(curve.epsilon2())},
                         {"epsilon3", std::to_string(curve.epsilon3())},
                         {"min_l3_confidence",
                          std::to_string(config_.degradation.min_l3_confidence)},
                         {"min_l2_confidence",
                          std::to_string(config_.degradation.min_l2_confidence)},
                         {"min_l1_confidence",
                          std::to_string(config_.degradation.min_l1_confidence)},
                         {"tau_ms", std::to_string(curve.tau_ms())},
                         {"base_tau_ms", std::to_string(tau_metrics.base_tau_ms)},
                         {"q_max", std::to_string(tau_metrics.q_max)},
                         {"volume_24h_qty", std::to_string(tau_metrics.volume_24h_qty)},
                         {"raw_hourly_rate", std::to_string(tau_metrics.raw_hourly_rate)},
                         {"hourly_turnover_cap",
                          std::to_string(tau_metrics.hourly_turnover_cap)},
                         {"tau_adjustment_reason",
                          tau_metrics.capped_by_turnover
                              ? "volume_24h_turnover_cap"
                              : "config"},
                         {"schema_version", std::to_string(curve.schema_version())},
                         {"min_compatible_schema_version",
                          std::to_string(curve.min_compatible_schema_version())},
                         {"producer_version", curve.producer_version()},
                         {"model_config_version",
                          config_.liquidity_fob_versioning.model_config_version},
                         {"build_latency_ms", std::to_string(build_latency_ms)},
                         {"degradation_reason", degrade_reason},
                         {"amm_path", using_direct_amm ? "direct" : "virtual_lob"},
                         {"amm_pool_fee_rate",
                          std::to_string(amm_pool_fee_rate_applied)},
                         {"amm_gas_cost_quote",
                          std::to_string(amm_gas_cost_quote_applied)},
                         {"amm_execution_overhead_bps",
                          std::to_string(amm_execution_overhead_bps_applied)},
                         {"amm_slippage_multiplier",
                          std::to_string(amm_slippage_multiplier_applied)},
                         {"amm_effective_fee_rate",
                          std::to_string(amm_effective_fee_rate_applied)},
                         {"amm_direct_vs_virtual_price_bps",
                          std::to_string(std::max(0.0, direct_vs_virtual_price_bps))},
                         {"amm_direct_vs_virtual_cost_rel",
                          std::to_string(std::max(0.0, direct_vs_virtual_cost_rel))},
                         {"bid_q_points", std::to_string(curve.bid_curve().q_grid_size())},
                         {"ask_q_points", std::to_string(curve.ask_curve().q_grid_size())},
                         {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"},
                         {"published", ok ? "true" : "false"}});
  if (!ok) {
    return false;
  }

  if (synthetic_config.enabled && suppress_synthetic) {
    cex::common::log_json("WARN", "Skipped venue.synthetic for stale snapshot",
                          {{"service", "venues"},
                           {"component", "venue_liquidity_curve_builder"},
                           {"participant", "Venue Liquidity Curve Builder"},
                           {"stage", "skip_synthetic_flow_order"},
                           {"venue", curve.venue_id()},
                           {"symbol", curve.instrument().symbol()},
                           {"snapshot_id", curve.snapshot_id()},
                           {"reason", "stale_snapshot"},
                           {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
    return true;
  }

  if (published_curve != nullptr) {
    *published_curve = curve;
  }

  const bool synthetic_ok =
      PublishSyntheticFlowOrders(curve, key, synthetic_config, published_synthetics);
  if (!synthetic_ok) {
    std::lock_guard<std::mutex> lock(state_mu_);
    ++degradation_state_by_symbol_[symbol_key].consecutive_publish_errors;
    return false;
  }
  return true;
}

void LiquidityCurveProducer::ApplyLiquidityFobVersioning(
    fob::venue::v1::VenueLiquidityCurve* curve) const {
  if (curve == nullptr) return;

  const uint32_t schema_version =
      std::max<uint32_t>(1, config_.liquidity_fob_versioning.schema_version);
  const uint32_t min_compatible_schema_version = std::clamp<uint32_t>(
      std::max<uint32_t>(1, config_.liquidity_fob_versioning.min_compatible_schema_version),
      1, schema_version);
  const std::string producer_version =
      config_.liquidity_fob_versioning.producer_version.empty()
          ? "f11-curve-15"
          : config_.liquidity_fob_versioning.producer_version;

  curve->set_schema_version(schema_version);
  curve->set_min_compatible_schema_version(min_compatible_schema_version);
  curve->set_producer_version(producer_version);

  auto* tags = curve->mutable_meta()->mutable_tags();
  (*tags)["topic"] = config_.topic;
  (*tags)["payload_type"] = "fob.venue.v1.VenueLiquidityCurve";
  (*tags)["payload_format"] = "protobuf";
  (*tags)["schema_version"] = std::to_string(schema_version);
  (*tags)["min_compatible_schema_version"] =
      std::to_string(min_compatible_schema_version);
  (*tags)["producer_version"] = producer_version;
}

bool LiquidityCurveProducer::PublishLiquidityFob(
    const fob::venue::v1::VenueLiquidityCurve& curve,
    const std::string& key) const {
  if (publisher_ == nullptr) return false;
  return publisher_->Publish(config_.topic, key, cex::common::to_bytes(curve));
}

void LiquidityCurveProducer::ObserveExecution(
    const fob::execution::v1::ExecutionIntent& intent,
    const fob::execution::v1::ExecutionReport& report) {
  const std::string venue = report.venue().empty() ? intent.venue() : report.venue();
  std::string symbol;
  if (report.has_instrument() && !report.instrument().symbol().empty()) {
    symbol = report.instrument().symbol();
  } else if (intent.has_instrument()) {
    symbol = intent.instrument().symbol();
  }
  if (venue.empty() || symbol.empty()) return;

  const bool execution_failed = IsExecutionFailure(report);
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    auto& state = degradation_state_by_symbol_[SymbolKey(venue, symbol)];
    if (execution_failed) {
      ++state.consecutive_execution_errors;
    } else if (report.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED ||
               report.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED) {
      state.consecutive_execution_errors = 0;
    }
  }
  if (execution_failed || !config_.l3_impact.enabled) return;

  const fob::common::v1::Side side = intent.side();
  if (side != fob::common::v1::SIDE_BUY && side != fob::common::v1::SIDE_SELL) return;

  double executed_qty = 0.0;
  if (report.has_filled_qty()) {
    executed_qty = ProtoDecimalAsDouble(report.filled_qty());
  }
  if (executed_qty <= 0.0 && intent.has_target_qty()) {
    executed_qty = ProtoDecimalAsDouble(intent.target_qty());
  }
  if (!std::isfinite(executed_qty) || executed_qty <= 0.0) return;

  if (!report.has_average_price()) return;
  const double executed_price = ProtoDecimalAsDouble(report.average_price());
  if (!std::isfinite(executed_price) || executed_price <= 0.0) return;

  const std::string symbol_key = SymbolKey(venue, symbol);
  double reference_price = 0.0;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    const auto it = reference_mid_by_symbol_.find(symbol_key);
    if (it == reference_mid_by_symbol_.end()) return;
    reference_price = it->second;
  }
  if (!std::isfinite(reference_price) || reference_price <= 0.0) return;

  double relative_impact = 0.0;
  if (side == fob::common::v1::SIDE_BUY) {
    relative_impact = (executed_price - reference_price) / reference_price;
  } else {
    relative_impact = (reference_price - executed_price) / reference_price;
  }
  relative_impact = ClampFinite(relative_impact, 0.0, config_.l3_impact.max_relative_impact);

  const std::size_t max_history = std::max<std::size_t>(1, config_.l3_impact.max_history);
  const std::string side_key = SideKey(venue, symbol, side);

  std::lock_guard<std::mutex> lock(state_mu_);
  auto& history = impact_history_by_side_[side_key];
  history.push_back(ImpactSample{
      .qty = executed_qty,
      .relative_impact = relative_impact,
      .executed_vwap = executed_price,
  });
  if (history.size() > max_history) {
    history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - max_history));
  }
  impact_params_by_side_[side_key] = CalibrateImpactModel(history);
  cex::common::log_json("INFO", "Recorded L3 impact calibration sample",
                        {{"service", "venues"},
                         {"component", "venue_liquidity_curve_builder"},
                         {"participant", "Venue Liquidity Curve Builder"},
                         {"stage", "record_l3_execution_sample"},
                         {"venue", venue},
                         {"symbol", symbol},
                         {"side", side == fob::common::v1::SIDE_BUY ? "buy" : "sell"},
                         {"history_size", std::to_string(history.size())},
                         {"relative_impact", std::to_string(relative_impact)},
                         {"executed_qty", std::to_string(executed_qty)},
                         {"executed_price", std::to_string(executed_price)},
                         {"impact_model", ImpactModelToString(config_.l3_impact.model)},
                         {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"}});
}

std::string LiquidityCurveProducer::SideKey(const std::string& venue_id,
                                            const std::string& symbol,
                                            const fob::common::v1::Side side) {
  const std::string side_text =
      (side == fob::common::v1::SIDE_BUY) ? "BUY" :
      (side == fob::common::v1::SIDE_SELL) ? "SELL" : "UNKNOWN";
  return venue_id + "|" + symbol + "|" + side_text;
}

std::string LiquidityCurveProducer::SideKey(const std::string& venue_id,
                                            const std::string& symbol,
                                            const domain::ExecutionSide side) {
  return venue_id + "|" + symbol + "|" + (side == domain::ExecutionSide::kBuy ? "BUY" : "SELL");
}

std::string LiquidityCurveProducer::SymbolKey(const std::string& venue_id,
                                              const std::string& symbol) {
  return venue_id + "|" + symbol;
}

void LiquidityCurveProducer::UpdateReferenceMid(const fob::venue::v1::VenueSnapshot& snapshot) {
  if (!snapshot.has_mid_price()) return;
  const double mid = ProtoDecimalAsDouble(snapshot.mid_price());
  if (!std::isfinite(mid) || mid <= 0.0) return;

  std::lock_guard<std::mutex> lock(state_mu_);
  reference_mid_by_symbol_[SymbolKey(snapshot.venue_id(), snapshot.instrument().symbol())] = mid;
}

LiquidityCurveProducer::ImpactModelParams LiquidityCurveProducer::CalibrateImpactModel(
    const std::vector<ImpactSample>& history) const {
  ImpactModelParams params;
  params.sample_count = history.size();
  if (history.empty()) return params;

  const auto fit_one_parameter = [&history](const auto x_fn) {
    double sxy = 0.0;
    double sxx = 0.0;
    for (const auto& sample : history) {
      const double x = x_fn(sample.qty);
      const double y = sample.relative_impact;
      sxy += x * y;
      sxx += x * x;
    }
    if (sxx <= 1e-12) return 0.0;
    return sxy / sxx;
  };

  switch (config_.l3_impact.model) {
    case L3ImpactModel::kLinear: {
      params.a = fit_one_parameter([](const double q) { return q; });
      params.b = 0.0;
      break;
    }
    case L3ImpactModel::kSqrt: {
      params.a = fit_one_parameter([](const double q) { return std::sqrt(std::max(0.0, q)); });
      params.b = 0.0;
      break;
    }
    case L3ImpactModel::kQuadratic: {
      double s11 = 0.0;
      double s12 = 0.0;
      double s22 = 0.0;
      double t1 = 0.0;
      double t2 = 0.0;
      for (const auto& sample : history) {
        const double q = std::max(0.0, sample.qty);
        const double q2 = q * q;
        s11 += q * q;
        s12 += q * q2;
        s22 += q2 * q2;
        t1 += q * sample.relative_impact;
        t2 += q2 * sample.relative_impact;
      }
      const double det = s11 * s22 - s12 * s12;
      if (std::fabs(det) > 1e-18) {
        params.a = (t1 * s22 - t2 * s12) / det;
        params.b = (s11 * t2 - s12 * t1) / det;
      } else {
        params.a = fit_one_parameter([](const double q) { return q; });
        params.b = 0.0;
      }
      break;
    }
  }

  params.a = std::max(0.0, params.a);
  params.b = std::max(0.0, params.b);

  double mse = 0.0;
  for (const auto& sample : history) {
    const double predicted = ClampFinite(
        EvaluateImpact(params, config_.l3_impact.model, sample.qty),
        0.0, config_.l3_impact.max_relative_impact);
    const double diff = predicted - sample.relative_impact;
    mse += diff * diff;
  }
  params.fit_rmse = std::sqrt(mse / static_cast<double>(history.size()));
  if (!std::isfinite(params.fit_rmse)) params.fit_rmse = 1.0;
  return params;
}

double LiquidityCurveProducer::EvaluateImpact(const ImpactModelParams& params,
                                              const L3ImpactModel model,
                                              const double qty) {
  const double q = std::max(0.0, qty);
  switch (model) {
    case L3ImpactModel::kLinear:
      return params.a * q;
    case L3ImpactModel::kQuadratic:
      return params.a * q + params.b * q * q;
    case L3ImpactModel::kSqrt:
      return params.a * std::sqrt(q);
  }
  return 0.0;
}

double LiquidityCurveProducer::ComputeBlendWeight(
    const ImpactModelParams& params,
    const L3ImpactCalibrationConfig& config) {
  const double base_w = ClampFinite(config.execution_blend_weight, 0.0, 1.0);
  if (base_w <= 0.0) return 0.0;
  const double n = static_cast<double>(params.sample_count);
  const double full_n = static_cast<double>(std::max<std::size_t>(1, config.min_samples_for_full_weight));
  const double warmup = std::clamp(n / full_n, 0.0, 1.0);
  return base_w * warmup;
}

domain::DepthSideCurves LiquidityCurveProducer::RebuildWithAdjustedMarginalPrice(
    const domain::DepthSideCurves& curves,
    const ImpactModelParams& params,
    const L3ImpactCalibrationConfig& config) {
  if (curves.s_of_q.size() < 2 || curves.p_of_q.size() != curves.s_of_q.size()) return curves;

  int32_t requested_cost_scale = 6;
  for (const auto& point : curves.s_of_q) {
    requested_cost_scale = std::max(requested_cost_scale, point.cumulative_cost.scale);
  }

  std::vector<double> execution_costs(curves.s_of_q.size(), 0.0);
  execution_costs[0] = DecimalAsDouble(curves.s_of_q.front().cumulative_cost);
  for (std::size_t i = 1; i < execution_costs.size(); ++i) {
    const double q_prev = DecimalAsDouble(curves.s_of_q[i - 1].qty);
    const double q_curr = DecimalAsDouble(curves.s_of_q[i].qty);
    const double dq = q_curr - q_prev;
    if (dq <= 0.0) {
      execution_costs[i] = execution_costs[i - 1];
      continue;
    }

    const double base_price = DecimalAsDouble(curves.p_of_q[i].price);
    const double rel_impact = ClampFinite(
        EvaluateImpact(params, config.model, q_curr),
        0.0,
        config.max_relative_impact);
    const double factor = (curves.side == domain::ExecutionSide::kBuy)
        ? (1.0 + rel_impact)
        : std::max(0.0, 1.0 - rel_impact);
    const double adjusted_price = std::max(0.0, base_price * factor);

    execution_costs[i] = execution_costs[i - 1] + adjusted_price * dq;
  }

  double max_abs_cost = 0.0;
  for (std::size_t i = 0; i < curves.s_of_q.size(); ++i) {
    max_abs_cost = std::max(max_abs_cost, std::fabs(DecimalAsDouble(curves.s_of_q[i].cumulative_cost)));
  }
  for (const double execution_cost : execution_costs) {
    max_abs_cost = std::max(max_abs_cost, std::fabs(execution_cost));
  }
  const int32_t cost_scale = FitScaleToMagnitude(max_abs_cost, requested_cost_scale);

  std::vector<domain::SOfQPoint> execution_based = curves.s_of_q;
  for (std::size_t i = 0; i < execution_based.size(); ++i) {
    execution_based[i].cumulative_cost =
        DoubleToDecimalWithScale(execution_costs[i], cost_scale);
  }

  const double blend_w = ComputeBlendWeight(params, config);
  if (blend_w <= 0.0) return curves;
  if (blend_w >= 1.0) {
    return domain::RebuildFromCostLayer(curves, execution_based);
  }

  std::vector<domain::SOfQPoint> mixed = curves.s_of_q;
  for (std::size_t i = 0; i < mixed.size(); ++i) {
    const double model_cost = DecimalAsDouble(curves.s_of_q[i].cumulative_cost);
    const double execution_cost = DecimalAsDouble(execution_based[i].cumulative_cost);
    mixed[i].cumulative_cost = DoubleToDecimalWithScale(
        (1.0 - blend_w) * model_cost + blend_w * execution_cost,
        cost_scale);
  }
  return domain::RebuildFromCostLayer(curves, mixed);
}

domain::DepthSideCurves LiquidityCurveProducer::ApplyL3ImpactCalibration(
    const domain::DepthSideCurves& curves,
    const std::string& venue_id,
    const std::string& symbol,
    const domain::ExecutionSide side) const {
  if (!config_.l3_impact.enabled) return curves;
  ImpactModelParams params;
  bool has_params = false;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    const auto it = impact_params_by_side_.find(SideKey(venue_id, symbol, side));
    if (it != impact_params_by_side_.end() && it->second.sample_count > 0) {
      params = it->second;
      has_params = true;
    }
  }
  if (!has_params) return curves;

  domain::DepthSideCurves out = RebuildWithAdjustedMarginalPrice(curves, params, config_.l3_impact);
  if (config_.apply_fenchel_legendre) {
    out = domain::ApplyFenchelLegendreLayer(out, config_.fenchel_legendre);
  }
  return out;
}

double LiquidityCurveProducer::ComputeExecutionCalibrationError(
    const std::string& venue_id,
    const std::string& symbol,
    const domain::ExecutionSide side) const {
  ImpactModelParams params;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    const auto it = impact_params_by_side_.find(SideKey(venue_id, symbol, side));
    if (it == impact_params_by_side_.end() || it->second.sample_count == 0) {
      return 1.0;
    }
    params = it->second;
  }

  const double impact_scale = std::max(1e-9, config_.l3_impact.max_relative_impact);
  const double normalized_rmse = std::clamp(params.fit_rmse / impact_scale, 0.0, 1.0);
  const double sample_penalty =
      1.0 / std::sqrt(static_cast<double>(std::max<std::size_t>(1, params.sample_count)));
  const double blend_weight = ComputeBlendWeight(params, config_.l3_impact);
  const double blend_penalty = 1.0 - blend_weight;

  return std::clamp(
      0.6 * normalized_rmse + 0.2 * sample_penalty + 0.2 * blend_penalty,
      0.0,
      1.0);
}

double LiquidityCurveProducer::ComputeExecutionCalibrationErrorBps(
    const domain::DepthSideCurves& curves,
    const std::string& venue_id,
    const std::string& symbol,
    const domain::ExecutionSide side) const {
  std::vector<ImpactSample> history;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    const auto it = impact_history_by_side_.find(SideKey(venue_id, symbol, side));
    if (it != impact_history_by_side_.end()) {
      history = it->second;
    }
  }
  if (history.empty()) return 0.0;
  if (curves.s_of_q.size() < 2) return 1.0e9;

  const auto interpolate_cost = [](const std::vector<domain::SOfQPoint>& s_of_q,
                                   const double qty) {
    if (s_of_q.empty()) return 0.0;
    if (qty <= DecimalAsDouble(s_of_q.front().qty)) {
      return DecimalAsDouble(s_of_q.front().cumulative_cost);
    }
    for (std::size_t i = 1; i < s_of_q.size(); ++i) {
      const double q0 = DecimalAsDouble(s_of_q[i - 1].qty);
      const double q1 = DecimalAsDouble(s_of_q[i].qty);
      if (qty <= q1) {
        const double s0 = DecimalAsDouble(s_of_q[i - 1].cumulative_cost);
        const double s1 = DecimalAsDouble(s_of_q[i].cumulative_cost);
        const double t = (q1 > q0) ? ((qty - q0) / (q1 - q0)) : 0.0;
        return s0 + std::clamp(t, 0.0, 1.0) * (s1 - s0);
      }
    }
    return DecimalAsDouble(s_of_q.back().cumulative_cost);
  };

  const double s0 = DecimalAsDouble(curves.s_of_q.front().cumulative_cost);
  double sq = 0.0;
  std::size_t count = 0;
  for (const auto& sample : history) {
    if (!std::isfinite(sample.qty) || sample.qty <= 0.0 ||
        !std::isfinite(sample.executed_vwap) || sample.executed_vwap <= 0.0) {
      continue;
    }
    const double s_q = interpolate_cost(curves.s_of_q, sample.qty);
    const double vwap_model = (s_q - s0) / sample.qty;
    if (!std::isfinite(vwap_model) || vwap_model <= 0.0) continue;

    const double err_bps = (vwap_model - sample.executed_vwap) /
                           sample.executed_vwap * 1.0e4;
    sq += err_bps * err_bps;
    ++count;
  }

  if (count == 0) return 0.0;
  return std::sqrt(sq / static_cast<double>(count));
}

bool LiquidityCurveProducer::HasExecutionSamples(
    const std::string& venue_id,
    const std::string& symbol,
    const domain::ExecutionSide side) const {
  std::lock_guard<std::mutex> lock(state_mu_);
  const auto it = impact_history_by_side_.find(SideKey(venue_id, symbol, side));
  return it != impact_history_by_side_.end() && !it->second.empty();
}

std::vector<domain::BookLevel> LiquidityCurveProducer::ToBookLevels(
    const google::protobuf::RepeatedPtrField<fob::common::v1::Decimal>& prices,
    const google::protobuf::RepeatedPtrField<fob::common::v1::Decimal>& quantities) {
  std::vector<domain::BookLevel> out;
  const int count = std::min(prices.size(), quantities.size());
  out.reserve(static_cast<std::size_t>(std::max(0, count)));

  for (int i = 0; i < count; ++i) {
    const cex::common::Decimal price = cex::common::Decimal::from_proto(prices.Get(i));
    const cex::common::Decimal qty = cex::common::Decimal::from_proto(quantities.Get(i));
    if (price.units <= 0 || qty.units <= 0) continue;
    out.push_back(domain::BookLevel{
        .price = price,
        .qty = qty,
    });
  }
  return out;
}

fob::venue::v1::SideLiquidityCurve LiquidityCurveProducer::ToProtoCurve(
    const domain::DepthSideCurves& curves) {
  fob::venue::v1::SideLiquidityCurve out;

  for (const auto& p : curves.p_of_q) {
    out.add_q_grid(DecimalAsDouble(p.qty));
    out.add_p_of_q(DecimalAsDouble(p.price));
  }
  for (const auto& p : curves.s_of_q) {
    out.add_s_of_q(DecimalAsDouble(p.cumulative_cost));
  }

  const auto& l_source = curves.l_of_v_monotone.empty() ? curves.l_of_v : curves.l_of_v_monotone;
  for (const auto& p : l_source) {
    out.add_v_grid(DecimalAsDouble(p.speed));
    out.add_l_of_v(DecimalAsDouble(p.lagrangian));
  }
  for (const auto& p : curves.l_of_v_monotone) {
    out.add_l_of_v_monotone(DecimalAsDouble(p.lagrangian));
  }

  if (!curves.fenchel_legendre.empty()) {
    const std::size_t n = std::min(curves.fenchel_legendre.s_star_of_p.size(),
                                   curves.fenchel_legendre.q_star_of_p.size());
    for (std::size_t i = 0; i < n; ++i) {
      out.add_p_star_grid(DecimalAsDouble(curves.fenchel_legendre.s_star_of_p[i].price));
      out.add_s_star_of_p(DecimalAsDouble(curves.fenchel_legendre.s_star_of_p[i].dual_value));
      out.add_q_star_of_p(DecimalAsDouble(curves.fenchel_legendre.q_star_of_p[i].optimal_qty));
    }
  }

  return out;
}

std::vector<fob::orders::v1::SyntheticFlowOrder>
LiquidityCurveProducer::BuildSyntheticFlowOrders(
    const fob::venue::v1::VenueLiquidityCurve& curve,
    const SyntheticFlowOrderConfig& synthetic_config) const {
  std::vector<fob::orders::v1::SyntheticFlowOrder> out;
  if (!synthetic_config.enabled) {
    return out;
  }
  if (curve.venue_id().empty() || curve.instrument().symbol().empty()) {
    return out;
  }

  const double tau_sec = SafePositiveScale(
      curve.tau_ms() / 1000.0,
      SafePositiveScale(config_.tau_ms / 1000.0, 1.0));
  const std::string curve_id =
      !curve.curve_id().empty()
          ? curve.curve_id()
          : (curve.has_meta() && !curve.meta().event_id().empty())
          ? curve.meta().event_id()
          : cex::common::uuid_v4();
  const std::string liquidity_source = InferLiquiditySource(
      curve.venue_id(), synthetic_config.liquidity_source);
  const google::protobuf::Timestamp created_at = cex::common::now_ts();
  const google::protobuf::Timestamp expires_at = AddMilliseconds(
      created_at, synthetic_config.ttl_ms);

  const auto append_order = [&](const fob::venue::v1::SideLiquidityCurve& side_curve,
                                const fob::common::v1::Side side,
                                const std::string& side_tag) {
    const std::optional<SyntheticCurveStats> stats =
        ExtractSyntheticCurveStats(side_curve);
    if (!stats.has_value()) {
      return;
    }

    const double q_rate = stats->q_max / tau_sec;
    if (!std::isfinite(q_rate) || q_rate <= 0.0) {
      return;
    }

    fob::orders::v1::SyntheticFlowOrder synthetic;
    const std::string synthetic_id = cex::common::uuid_v4();
    synthetic.set_synthetic_id(synthetic_id);
    synthetic.set_venue_id(curve.venue_id());
    *synthetic.mutable_instrument() = curve.instrument();
    synthetic.set_side(side);
    *synthetic.mutable_p_l() =
        DoubleToDecimalWithScale(stats->p_low, synthetic_config.price_scale).to_proto();
    *synthetic.mutable_p_h() =
        DoubleToDecimalWithScale(stats->p_high, synthetic_config.price_scale).to_proto();
    *synthetic.mutable_q_rate() =
        DoubleToDecimalWithScale(q_rate, synthetic_config.quantity_scale).to_proto();
    *synthetic.mutable_q_max() =
        DoubleToDecimalWithScale(stats->q_max, synthetic_config.quantity_scale).to_proto();
    synthetic.set_curve_id(curve_id);
    synthetic.set_snapshot_id(curve.snapshot_id());
    synthetic.set_liquidity_source(liquidity_source);
    *synthetic.mutable_created_at() = created_at;
    *synthetic.mutable_expires_at() = expires_at;
    synthetic.set_status("active");

    auto* order = synthetic.mutable_order();
    order->set_order_id(synthetic_id);
    order->set_client_order_id(curve_id + ":" + side_tag);
    order->set_user_id(liquidity_source);
    order->set_account_id(curve.venue_id());
    *order->mutable_instrument() = curve.instrument();
    order->set_side(side);
    *order->mutable_total_qty() = synthetic.q_max();
    *order->mutable_remaining_qty() = synthetic.q_max();
    *order->mutable_price_low() = synthetic.p_l();
    *order->mutable_price_high() = synthetic.p_h();
    *order->mutable_max_speed() = synthetic.q_rate();
    // ORDER_STATUS_UNSPECIFIED maps to an active internal FlowOrder in matching.
    order->set_status(fob::common::v1::ORDER_STATUS_UNSPECIFIED);
    *order->mutable_created_at() = created_at;
    *order->mutable_updated_at() = created_at;
    order->set_tif(fob::common::v1::TIF_GTC);
    (*order->mutable_tags())["synthetic"] = "true";
    (*order->mutable_tags())["venue_id"] = curve.venue_id();
    (*order->mutable_tags())["curve_id"] = curve_id;
    (*order->mutable_tags())["snapshot_id"] = curve.snapshot_id();
    (*order->mutable_tags())["liquidity_source"] = liquidity_source;
    (*order->mutable_tags())["status"] = "active";

    out.push_back(std::move(synthetic));
  };

  if (curve.has_bid_curve()) {
    append_order(curve.bid_curve(), fob::common::v1::SIDE_BUY, "bid");
  }
  if (curve.has_ask_curve()) {
    append_order(curve.ask_curve(), fob::common::v1::SIDE_SELL, "ask");
  }

  return out;
}

bool LiquidityCurveProducer::PublishSyntheticFlowOrders(
    const fob::venue::v1::VenueLiquidityCurve& curve,
    const std::string& key,
    const SyntheticFlowOrderConfig& synthetic_config,
    std::vector<fob::orders::v1::SyntheticFlowOrder>* published_orders) {
  if (!synthetic_config.enabled) {
    if (published_orders != nullptr) {
      published_orders->clear();
    }
    return true;
  }
  if (publisher_ == nullptr) {
    if (published_orders != nullptr) {
      published_orders->clear();
    }
    return false;
  }

  const auto orders = BuildSyntheticFlowOrders(curve, synthetic_config);
  if (published_orders != nullptr) {
    *published_orders = orders;
  }
  if (orders.empty()) {
    cex::common::log_json("WARN", "No SyntheticFlowOrder generated",
                          {{"venue", curve.venue_id()},
                           {"symbol", curve.instrument().symbol()},
                           {"curve_id", curve.has_meta() ? curve.meta().event_id() : ""}});
    return false;
  }

  bool ok = true;
  for (const auto& order : orders) {
    const bool published = publisher_->Publish(
        synthetic_config.topic, key, cex::common::to_bytes(order));
    bool stored = true;
    if (synthetic_order_repository_ != nullptr) {
      stored = synthetic_order_repository_->SaveSyntheticOrder(order);
    }

    cex::common::log_json(published && stored ? "INFO" : "ERROR",
                          "Published venue.synthetic",
                          {{"service", "venues"},
                           {"component", "venue_liquidity_curve_builder"},
                           {"participant", "Venue Liquidity Curve Builder"},
                           {"stage", "publish_synthetic_flow_order"},
                           {"venue", order.venue_id()},
                           {"symbol", order.instrument().symbol()},
                           {"side", SideText(order.side())},
                           {"synthetic_id", order.synthetic_id()},
                           {"curve_id", order.curve_id()},
                           {"snapshot_id", order.snapshot_id()},
                           {"p_l",
                            cex::common::Decimal::from_proto(order.p_l()).to_string()},
                           {"p_h",
                            cex::common::Decimal::from_proto(order.p_h()).to_string()},
                           {"q_rate",
                            cex::common::Decimal::from_proto(order.q_rate()).to_string()},
                           {"q_max",
                            cex::common::Decimal::from_proto(order.q_max()).to_string()},
                           {"liquidity_source", order.liquidity_source()},
                           {"topic", synthetic_config.topic},
                           {"source_file", "cpp/venues/src/app/liquidity_curve_producer.cpp"},
                           {"published", published ? "true" : "false"},
                           {"stored", stored ? "true" : "false"}});
    ok = (published && stored) && ok;
  }

  return ok;
}

domain::DepthSideCurves LiquidityCurveProducer::ApplyL2Pipeline(
    domain::DepthSideCurves curves,
    const LiquidityCurveProducerConfig& config) {
  if (config.apply_convexification) {
    curves = domain::ApplyConvexifiedCostLayer(curves, config.convexification);
  }
  if (config.apply_moreau_l2) {
    curves = domain::ApplyMoreauRegularizationL2(curves, config.moreau_l2);
  }
  if (config.apply_tikhonov_l2) {
    curves = domain::ApplyTikhonovRegularizationL2(curves, config.tikhonov_l2);
  }
  if (config.apply_fenchel_legendre) {
    curves = domain::ApplyFenchelLegendreLayer(curves, config.fenchel_legendre);
  }
  return curves;
}

double LiquidityCurveProducer::ComputeRelativeCostError(
    const std::vector<domain::SOfQPoint>& base,
    const std::vector<domain::SOfQPoint>& regularized) {
  const std::size_t n = std::min(base.size(), regularized.size());
  if (n == 0) return 1.0;

  double num = 0.0;
  double den = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double b = DecimalAsDouble(base[i].cumulative_cost);
    const double r = DecimalAsDouble(regularized[i].cumulative_cost);
    const double d = r - b;
    num += d * d;

    const double norm = std::max(1.0, std::fabs(b));
    den += norm * norm;
  }
  if (den <= 1e-12) return 0.0;
  return std::sqrt(num / den);
}

double LiquidityCurveProducer::ComputePriceErrorBps(
    const std::vector<domain::POfQPoint>& base,
    const std::vector<domain::POfQPoint>& adjusted) {
  const std::size_t n = std::min(base.size(), adjusted.size());
  if (n == 0) return 1.0e9;

  double sq = 0.0;
  double avg_price = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double p_base = DecimalAsDouble(base[i].price);
    const double p_adjusted = DecimalAsDouble(adjusted[i].price);
    const double diff = p_adjusted - p_base;
    sq += diff * diff;
    avg_price += std::fabs(p_base);
  }

  const double eps_abs = std::sqrt(sq / static_cast<double>(n));
  avg_price /= static_cast<double>(n);
  if (avg_price <= 1e-12) return eps_abs;
  return eps_abs / avg_price * 1.0e4;
}

double LiquidityCurveProducer::ComputeMonotonicityError(
    const std::vector<domain::POfQPoint>& p_of_q,
    const domain::ExecutionSide side) {
  if (p_of_q.size() < 2) return 1.0;

  double max_violation = 0.0;
  for (std::size_t i = 1; i < p_of_q.size(); ++i) {
    const double prev = DecimalAsDouble(p_of_q[i - 1].price);
    const double curr = DecimalAsDouble(p_of_q[i].price);
    const double delta = curr - prev;
    double violation = 0.0;
    if (side == domain::ExecutionSide::kBuy) {
      violation = std::max(0.0, -delta);
    } else {
      violation = std::max(0.0, delta);
    }
    max_violation = std::max(max_violation, violation);
  }

  const double first = DecimalAsDouble(p_of_q.front().price);
  const double last = DecimalAsDouble(p_of_q.back().price);
  const double range = std::max(1.0, std::fabs(last - first));
  return max_violation / range;
}

double LiquidityCurveProducer::ComputeConvexityError(
    const std::vector<domain::SOfQPoint>& s_of_q,
    const domain::ExecutionSide side) {
  if (s_of_q.size() < 3) return 0.0;

  double max_violation = 0.0;
  bool has_valid_triplet = false;
  for (std::size_t i = 1; i + 1 < s_of_q.size(); ++i) {
    const double q_prev = DecimalAsDouble(s_of_q[i - 1].qty);
    const double q_curr = DecimalAsDouble(s_of_q[i].qty);
    const double q_next = DecimalAsDouble(s_of_q[i + 1].qty);

    const double dq1 = q_curr - q_prev;
    const double dq2 = q_next - q_curr;
    if (dq1 <= 0.0 || dq2 <= 0.0) continue;
    has_valid_triplet = true;

    const double s_prev = DecimalAsDouble(s_of_q[i - 1].cumulative_cost);
    const double s_curr = DecimalAsDouble(s_of_q[i].cumulative_cost);
    const double s_next = DecimalAsDouble(s_of_q[i + 1].cumulative_cost);

    const double slope1 = (s_curr - s_prev) / dq1;
    const double slope2 = (s_next - s_curr) / dq2;
    const double violation = (side == domain::ExecutionSide::kBuy)
        ? std::max(0.0, slope1 - slope2)
        : std::max(0.0, slope2 - slope1);
    const double scale = std::max({1.0, std::fabs(slope1), std::fabs(slope2)});
    max_violation = std::max(max_violation, violation / scale);
  }
  if (!has_valid_triplet) return 0.0;
  return max_violation;
}

double LiquidityCurveProducer::ComputeShapeErrorBps(
    const std::vector<domain::POfQPoint>& p_of_q,
    const std::vector<domain::SOfQPoint>& s_of_q,
    const domain::ExecutionSide side) {
  if (p_of_q.size() < 2 && s_of_q.size() < 2) return 1.0e9;

  double max_violation = 0.0;
  double avg_price = 0.0;
  std::size_t price_count = 0;

  for (std::size_t i = 1; i < p_of_q.size(); ++i) {
    const double prev = DecimalAsDouble(p_of_q[i - 1].price);
    const double curr = DecimalAsDouble(p_of_q[i].price);
    if (!std::isfinite(prev) || !std::isfinite(curr)) continue;

    const double violation = (side == domain::ExecutionSide::kBuy)
        ? std::max(0.0, prev - curr)
        : std::max(0.0, curr - prev);
    max_violation = std::max(max_violation, violation);
    avg_price += std::fabs(curr);
    ++price_count;
  }

  for (std::size_t i = 1; i < s_of_q.size(); ++i) {
    const double q_prev = DecimalAsDouble(s_of_q[i - 1].qty);
    const double q_curr = DecimalAsDouble(s_of_q[i].qty);
    const double dq = q_curr - q_prev;
    if (dq <= 1e-12) continue;

    const double s_prev = DecimalAsDouble(s_of_q[i - 1].cumulative_cost);
    const double s_curr = DecimalAsDouble(s_of_q[i].cumulative_cost);
    const double ds = s_curr - s_prev;
    const double marginal = ds / dq;
    if (!std::isfinite(marginal)) continue;
    avg_price += std::fabs(marginal);
    ++price_count;

    if (ds < 0.0) {
      max_violation = std::max(max_violation, -ds / dq);
    }
  }

  if (price_count == 0) return 1.0e9;
  avg_price /= static_cast<double>(price_count);
  if (avg_price <= 1e-12) return max_violation;
  return max_violation / avg_price * 1.0e4;
}

double LiquidityCurveProducer::ComputeDualPenalty(const domain::DepthSideCurves& curves) {
  if (curves.fenchel_legendre.empty()) return 1.0;

  const auto& q_star = curves.fenchel_legendre.q_star_of_p;
  if (q_star.size() < 2) return 1.0;

  double max_violation = 0.0;
  for (std::size_t i = 1; i < q_star.size(); ++i) {
    const double prev = DecimalAsDouble(q_star[i - 1].optimal_qty);
    const double curr = DecimalAsDouble(q_star[i].optimal_qty);
    max_violation = std::max(max_violation, std::max(0.0, prev - curr));
  }

  const double span = std::max(
      1.0,
      std::fabs(DecimalAsDouble(q_star.back().optimal_qty) -
                DecimalAsDouble(q_star.front().optimal_qty)));
  return max_violation / span;
}

double LiquidityCurveProducer::Clamp01(const double value) {
  if (!std::isfinite(value)) return 0.0;
  return std::clamp(value, 0.0, 1.0);
}

}  // namespace cex::venues::app
