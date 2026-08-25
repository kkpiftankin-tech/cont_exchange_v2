#include "infra/simulated_venue_adapter.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "cex/common/time.hpp"

namespace cex::venues::infra {

namespace {

struct SimulatedVenueProfile {
  int64_t base_mid_units{6840000};
  int64_t primary_wave_units{2200};
  int64_t secondary_wave_units{850};
  int64_t base_half_spread_units{80};
  int64_t spread_wave_units{40};
  int64_t price_step_units{10};
  int64_t base_qty_units{4200};
  int64_t qty_wave_units{650};
  int64_t qty_decay_units{180};
  int64_t base_volume_units{18000000};
  int64_t volume_wave_units{1800000};
  int32_t price_scale{2};
  int32_t qty_scale{3};
  cex::common::Decimal maker_fee{1, 3};
  cex::common::Decimal taker_fee{1, 3};
  cex::common::Decimal tick_size{10, 2};
  cex::common::Decimal lot_size{1, 4};
  cex::common::Decimal min_notional{1000, 2};
  cex::common::Decimal min_qty{1, 4};
  cex::common::Decimal max_qty{2500000, 4};
  uint32_t phase_seed{1};
};

cex::common::Decimal dec_from_int(const int64_t units, const int32_t scale) {
  return cex::common::Decimal{
      .units = units,
      .scale = scale,
  };
}

fob::common::v1::Instrument make_default_instrument() {
  fob::common::v1::Instrument instrument;
  instrument.set_symbol("BTC/USDT");
  instrument.set_base("BTC");
  instrument.set_quote("USDT");
  return instrument;
}

fob::common::v1::Instrument ensure_instrument_fields(
    fob::common::v1::Instrument instrument) {
  if (instrument.symbol().empty() && !instrument.base().empty() &&
      !instrument.quote().empty()) {
    instrument.set_symbol(instrument.base() + "/" + instrument.quote());
  }

  if (!instrument.symbol().empty() &&
      (instrument.base().empty() || instrument.quote().empty())) {
    const auto slash = instrument.symbol().find('/');
    if (slash != std::string::npos) {
      if (instrument.base().empty()) {
        instrument.set_base(instrument.symbol().substr(0, slash));
      }
      if (instrument.quote().empty()) {
        instrument.set_quote(instrument.symbol().substr(slash + 1));
      }
    }
  }

  if (instrument.symbol().empty()) {
    return make_default_instrument();
  }
  return instrument;
}

std::string derive_venue_symbol(const fob::common::v1::Instrument& instrument) {
  if (!instrument.base().empty() && !instrument.quote().empty()) {
    return instrument.base() + instrument.quote();
  }

  std::string symbol = instrument.symbol();
  symbol.erase(
      std::remove(symbol.begin(), symbol.end(), '/'),
      symbol.end());

  if (symbol.empty()) return "BTCUSDT";
  return symbol;
}

std::string to_lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

int64_t round_to_step(const int64_t units, const int64_t step_units) {
  if (step_units <= 1) return units;
  return ((units + (step_units / 2)) / step_units) * step_units;
}

int64_t wave_units(const uint64_t sequence,
                   const uint32_t phase_seed,
                   const double speed,
                   const int64_t amplitude,
                   const bool use_sine) {
  if (amplitude == 0) return 0;
  const double phase = static_cast<double>(sequence + phase_seed);
  const double oscillation = use_sine ? std::sin(phase * speed) : std::cos(phase * speed);
  return static_cast<int64_t>(std::llround(oscillation * static_cast<double>(amplitude)));
}

SimulatedVenueProfile build_venue_profile(const std::string& venue_id,
                                          const domain::VenueType venue_type) {
  const std::string venue_key = to_lower_copy(venue_id);
  if (venue_key.find("coinbase") != std::string::npos) {
    return SimulatedVenueProfile{
        .base_mid_units = 6845560,
        .primary_wave_units = 1850,
        .secondary_wave_units = 620,
        .base_half_spread_units = 95,
        .spread_wave_units = 30,
        .price_step_units = 1,
        .base_qty_units = 1950,
        .qty_wave_units = 240,
        .qty_decay_units = 85,
        .base_volume_units = 12940300,
        .volume_wave_units = 920000,
        .price_scale = 2,
        .qty_scale = 3,
        .maker_fee = dec_from_int(12, 4),
        .taker_fee = dec_from_int(14, 4),
        .tick_size = dec_from_int(1, 2),
        .lot_size = dec_from_int(1, 5),
        .min_notional = dec_from_int(100, 1),
        .min_qty = dec_from_int(1, 5),
        .max_qty = dec_from_int(1200000, 4),
        .phase_seed = 11,
    };
  }

  if (venue_key.find("uniswap") != std::string::npos ||
      venue_key.find("curve") != std::string::npos ||
      venue_type == domain::VenueType::kDex ||
      venue_type == domain::VenueType::kAmm) {
    return SimulatedVenueProfile{
        .base_mid_units = 6842080,
        .primary_wave_units = 2650,
        .secondary_wave_units = 1180,
        .base_half_spread_units = 320,
        .spread_wave_units = 95,
        .price_step_units = 5,
        .base_qty_units = 860,
        .qty_wave_units = 140,
        .qty_decay_units = 42,
        .base_volume_units = 3875600,
        .volume_wave_units = 260000,
        .price_scale = 2,
        .qty_scale = 3,
        .maker_fee = dec_from_int(0, 4),
        .taker_fee = dec_from_int(30, 4),
        .tick_size = dec_from_int(1, 2),
        .lot_size = dec_from_int(1, 5),
        .min_notional = dec_from_int(2500, 2),
        .min_qty = dec_from_int(1, 5),
        .max_qty = dec_from_int(350000, 4),
        .phase_seed = 23,
    };
  }

  if (venue_key.find("binance") != std::string::npos) {
    return SimulatedVenueProfile{
        .base_mid_units = 6844210,
        .primary_wave_units = 2100,
        .secondary_wave_units = 780,
        .base_half_spread_units = 60,
        .spread_wave_units = 25,
        .price_step_units = 10,
        .base_qty_units = 4200,
        .qty_wave_units = 520,
        .qty_decay_units = 180,
        .base_volume_units = 43820500,
        .volume_wave_units = 1500000,
        .price_scale = 2,
        .qty_scale = 3,
        .maker_fee = dec_from_int(10, 4),
        .taker_fee = dec_from_int(10, 4),
        .tick_size = dec_from_int(10, 2),
        .lot_size = dec_from_int(1, 4),
        .min_notional = dec_from_int(1000, 2),
        .min_qty = dec_from_int(1, 4),
        .max_qty = dec_from_int(5000000, 4),
        .phase_seed = 5,
    };
  }

  return SimulatedVenueProfile{
      .base_mid_units = 6843700,
      .primary_wave_units = 1650,
      .secondary_wave_units = 550,
      .base_half_spread_units = 110,
      .spread_wave_units = 35,
      .price_step_units = 5,
      .base_qty_units = 1500,
      .qty_wave_units = 220,
      .qty_decay_units = 70,
      .base_volume_units = 9400500,
      .volume_wave_units = 640000,
      .price_scale = 2,
      .qty_scale = 3,
      .maker_fee = dec_from_int(12, 4),
      .taker_fee = dec_from_int(16, 4),
      .tick_size = dec_from_int(5, 2),
      .lot_size = dec_from_int(1, 4),
      .min_notional = dec_from_int(1500, 2),
      .min_qty = dec_from_int(1, 4),
      .max_qty = dec_from_int(1500000, 4),
      .phase_seed = 31,
  };
}

int64_t compute_mid_price_units(const SimulatedVenueProfile& profile,
                                const uint64_t sequence) {
  const int64_t drift =
      wave_units(sequence, profile.phase_seed, 0.43, profile.primary_wave_units, true) +
      wave_units(sequence, profile.phase_seed * 2, 0.16, profile.secondary_wave_units, false);
  return std::max<int64_t>(
      profile.price_step_units,
      round_to_step(profile.base_mid_units + drift, profile.price_step_units));
}

int64_t compute_half_spread_units(const SimulatedVenueProfile& profile,
                                  const uint64_t sequence) {
  const int64_t spread_breath =
      std::abs(wave_units(sequence, profile.phase_seed * 3, 0.29, profile.spread_wave_units, true));
  return std::max<int64_t>(profile.price_step_units, profile.base_half_spread_units + spread_breath);
}

int64_t compute_volume_units(const SimulatedVenueProfile& profile,
                             const uint64_t sequence) {
  const int64_t volume_drift =
      wave_units(sequence, profile.phase_seed * 5, 0.08, profile.volume_wave_units, false);
  return std::max<int64_t>(profile.base_volume_units / 2, profile.base_volume_units + volume_drift);
}

uint32_t compute_latency_ms(const SimulatedVenueProfile& profile,
                            const uint64_t sequence) {
  const int64_t base_latency =
      (profile.phase_seed % 2 == 0) ? 46 : 34;
  const int64_t venue_skew =
      std::max<int64_t>(0, profile.base_half_spread_units / std::max<int64_t>(1, profile.price_step_units));
  const int64_t dynamic =
      std::abs(wave_units(sequence, profile.phase_seed * 7, 0.19, 9 + venue_skew / 6, true));
  return static_cast<uint32_t>(std::max<int64_t>(1, base_latency + dynamic));
}

int64_t now_epoch_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

SimulatedVenueAdapter::SimulatedVenueAdapter(
    std::string venue_id,
    const domain::VenueType venue_type)
    : venue_id_(std::move(venue_id)),
      venue_type_(venue_type) {
  mid_price_units_ = build_venue_profile(venue_id_, venue_type_).base_mid_units;
}

std::string SimulatedVenueAdapter::VenueId() const {
  return venue_id_;
}

domain::VenueType SimulatedVenueAdapter::Type() const {
  return venue_type_;
}

bool SimulatedVenueAdapter::Connect() {
  std::lock_guard<std::mutex> lock(mu_);
  connected_ = true;
  consecutive_errors_ = 0;
  return true;
}

bool SimulatedVenueAdapter::Subscribe(
    const std::vector<domain::VenueSubscription>& subscriptions) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!connected_) {
    ++consecutive_errors_;
    return false;
  }
  subscriptions_ = subscriptions;
  consecutive_errors_ = 0;
  return true;
}

bool SimulatedVenueAdapter::Reconnect() {
  std::lock_guard<std::mutex> lock(mu_);
  ++reconnect_attempts_;
  connected_ = true;
  consecutive_errors_ = 0;
  return true;
}

domain::VenueHeartbeat SimulatedVenueAdapter::Heartbeat() {
  std::lock_guard<std::mutex> lock(mu_);
  const SimulatedVenueProfile profile = build_venue_profile(venue_id_, venue_type_);
  const int64_t now_ms = now_epoch_ms();
  const uint32_t latency_ms = compute_latency_ms(profile, sequence_);
  const uint32_t stale_ms = last_snapshot_time_ms_ > 0 && now_ms >= last_snapshot_time_ms_
      ? static_cast<uint32_t>(now_ms - last_snapshot_time_ms_)
      : 0U;
  return domain::VenueHeartbeat{
      .venue_id = venue_id_,
      .venue_type = venue_type_,
      .status = connected_ ? domain::VenueConnectionStatus::kConnected
                           : domain::VenueConnectionStatus::kDisconnected,
      .timestamp = cex::common::now_ts(),
      .latency_ms = latency_ms,
      .stale_ms = stale_ms,
      .reconnect_attempts = reconnect_attempts_,
      .consecutive_errors = consecutive_errors_,
      .last_sequence = sequence_,
  };
}

std::optional<domain::VenueRawSnapshot> SimulatedVenueAdapter::RequestSnapshot(
    const domain::VenueSnapshotRequest& request) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!connected_) {
    ++consecutive_errors_;
    return std::nullopt;
  }

  const std::size_t depth_levels = request.depth_levels == 0 ? 20 : request.depth_levels;
  const fob::common::v1::Instrument instrument = ResolveInstrument(request);
  const std::string venue_symbol = ResolveVenueSymbol(request);
  const SimulatedVenueProfile profile = build_venue_profile(venue_id_, venue_type_);

  ++sequence_;
  mid_price_units_ = compute_mid_price_units(profile, sequence_);
  const int64_t half_spread_units = compute_half_spread_units(profile, sequence_);

  domain::VenueRawSnapshot snapshot;
  snapshot.venue_id = venue_id_;
  snapshot.venue_type = venue_type_;
  snapshot.instrument = instrument;
  snapshot.venue_symbol = venue_symbol;
  snapshot.timestamp = cex::common::now_ts();
  snapshot.sequence = sequence_;
  snapshot.status = domain::VenueConnectionStatus::kConnected;
  snapshot.volume_24h = dec_from_int(compute_volume_units(profile, sequence_), profile.qty_scale);

  snapshot.fees.maker = profile.maker_fee;
  snapshot.fees.taker = profile.taker_fee;
  snapshot.trading_rules.tick_size = profile.tick_size;
  snapshot.trading_rules.lot_size = profile.lot_size;
  snapshot.trading_rules.min_notional = profile.min_notional;
  snapshot.trading_rules.min_qty = profile.min_qty;
  snapshot.trading_rules.max_qty = profile.max_qty;

  snapshot.bids.reserve(depth_levels);
  snapshot.asks.reserve(depth_levels);

  for (std::size_t i = 0; i < depth_levels; ++i) {
    const int64_t level_index = static_cast<int64_t>(i);
    const int64_t price_offset_units =
        half_spread_units + level_index * profile.price_step_units;
    const int64_t bid_price_units = std::max<int64_t>(
        profile.price_step_units,
        mid_price_units_ - price_offset_units);
    const int64_t ask_price_units = mid_price_units_ + price_offset_units;
    const int64_t bid_qty_units = std::max<int64_t>(
        25,
        profile.base_qty_units +
            wave_units(sequence_ + level_index, profile.phase_seed + static_cast<uint32_t>(i),
                       0.31, profile.qty_wave_units, true) -
            level_index * profile.qty_decay_units);
    const int64_t ask_qty_units = std::max<int64_t>(
        25,
        profile.base_qty_units +
            wave_units(sequence_ + level_index + 3,
                       profile.phase_seed + static_cast<uint32_t>(i) + 7,
                       0.27,
                       profile.qty_wave_units / 2,
                       false) -
            level_index * std::max<int64_t>(15, profile.qty_decay_units - 10));

    snapshot.bids.push_back(domain::VenueBookLevel{
        .price = dec_from_int(bid_price_units, profile.price_scale),
        .qty = dec_from_int(bid_qty_units, profile.qty_scale),
    });

    snapshot.asks.push_back(domain::VenueBookLevel{
        .price = dec_from_int(ask_price_units, profile.price_scale),
        .qty = dec_from_int(ask_qty_units, profile.qty_scale),
    });
  }

  snapshot.best_bid = snapshot.bids.front().price;
  snapshot.best_ask = snapshot.asks.front().price;
  snapshot.mid_price = dec_from_int(
      (snapshot.best_bid.units + snapshot.best_ask.units) / 2,
      snapshot.best_bid.scale);
  snapshot.spread = cex::common::Decimal::sub(snapshot.best_ask, snapshot.best_bid);
  last_snapshot_time_ms_ = now_epoch_ms();

  return snapshot;
}

domain::VenueOrderResult SimulatedVenueAdapter::SendOrder(
    const fob::execution::v1::ExecutionIntent& intent) {
  std::lock_guard<std::mutex> lock(mu_);

  domain::VenueOrderResult result;
  result.venue_order_id = "SIM-" +
      (intent.intent_id().empty() ? intent.client_order_id() : intent.intent_id());

  if (!connected_) {
    ++consecutive_errors_;
    result.accepted = false;
    result.status = fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
    result.error_code = "VENUE_DISCONNECTED";
    result.error_message = "Adapter is disconnected";
    return result;
  }

  result.accepted = true;
  result.status = fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
  result.filled_qty = cex::common::Decimal::from_proto(intent.target_qty());
  result.remaining_qty = dec_from_int(0, result.filled_qty.scale);
  if (intent.has_limit_price() && intent.limit_price().units() != 0) {
    result.average_price = cex::common::Decimal::from_proto(intent.limit_price());
  } else {
    const SimulatedVenueProfile profile = build_venue_profile(venue_id_, venue_type_);
    result.average_price = dec_from_int(mid_price_units_, profile.price_scale);
  }
  consecutive_errors_ = 0;
  return result;
}

bool SimulatedVenueAdapter::ApplyRuntimeConfig(
    const domain::VenueAdapterRuntimeConfig& config) {
  std::lock_guard<std::mutex> lock(mu_);
  if (config.stale_threshold_ms.has_value() ||
      config.circuit_breaker_enabled.has_value() ||
      config.circuit_breaker_errors.has_value() ||
      config.circuit_breaker_window_ms.has_value() ||
      config.circuit_breaker_cooldown_ms.has_value()) {
    // Simulated adapter keeps static behavior but accepts runtime updates
    // for API compatibility.
  }
  return true;
}

fob::common::v1::Instrument SimulatedVenueAdapter::ResolveInstrument(
    const domain::VenueSnapshotRequest& request) const {
  if (!request.instrument.symbol().empty() ||
      !request.instrument.base().empty() ||
      !request.instrument.quote().empty()) {
    return ensure_instrument_fields(request.instrument);
  }

  if (!subscriptions_.empty()) {
    return ensure_instrument_fields(subscriptions_.front().instrument);
  }

  return make_default_instrument();
}

std::string SimulatedVenueAdapter::ResolveVenueSymbol(
    const domain::VenueSnapshotRequest& request) const {
  if (!request.venue_symbol.empty()) {
    return request.venue_symbol;
  }

  if (!subscriptions_.empty() &&
      !subscriptions_.front().venue_symbol.empty()) {
    return subscriptions_.front().venue_symbol;
  }

  return derive_venue_symbol(ResolveInstrument(request));
}

}  // namespace cex::venues::infra
