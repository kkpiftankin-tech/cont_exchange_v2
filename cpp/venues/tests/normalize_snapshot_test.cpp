#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "domain/normalize_snapshot.hpp"
#include "domain/venue_adapter.hpp"

namespace {

using cex::common::Decimal;
using cex::venues::domain::NormalizationConfig;
using cex::venues::domain::NormalizeSnapshot;
using cex::venues::domain::QuantizeDecimal;
using cex::venues::domain::VenueBookLevel;
using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueFees;
using cex::venues::domain::VenueRawSnapshot;
using cex::venues::domain::VenueTradingRules;
using cex::venues::domain::VenueType;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool CheckDecimal(const Decimal& actual, int64_t units, int32_t scale,
                  const std::string& name) {
  if (actual.units == units && actual.scale == scale) return true;
  std::cerr << "[FAIL] " << name << " expected {" << units << ", " << scale
            << "} got {" << actual.units << ", " << actual.scale << "}"
            << std::endl;
  return false;
}

bool CheckProtoDecimal(const fob::common::v1::Decimal& proto, int64_t units,
                       int32_t scale, const std::string& name) {
  return CheckDecimal(Decimal::from_proto(proto), units, scale, name);
}

VenueRawSnapshot MakeRaw() {
  VenueRawSnapshot raw;
  raw.venue_id = "binance";
  raw.venue_type = VenueType::kCex;
  raw.instrument.set_symbol("BTC/USDT");
  raw.instrument.set_base("BTC");
  raw.instrument.set_quote("USDT");
  raw.timestamp.set_seconds(1700000000);
  raw.sequence = 42;
  raw.status = VenueConnectionStatus::kConnected;

  raw.best_bid = Decimal{7000000, 2};  // 70000.00
  raw.best_ask = Decimal{7001000, 2};  // 70010.00

  raw.bids = {
      VenueBookLevel{Decimal{7000000, 2}, Decimal{500, 3}},   // 70000.00 x 0.500
      VenueBookLevel{Decimal{6999000, 2}, Decimal{1000, 3}},  // 69990.00 x 1.000
  };
  raw.asks = {
      VenueBookLevel{Decimal{7001000, 2}, Decimal{300, 3}},   // 70010.00 x 0.300
      VenueBookLevel{Decimal{7002000, 2}, Decimal{800, 3}},   // 70020.00 x 0.800
  };

  raw.fees.maker = Decimal{100, 6};  // 0.000100 (1 bps)
  raw.fees.taker = Decimal{200, 6};  // 0.000200 (2 bps)

  raw.trading_rules.tick_size = Decimal{100, 2};  // 1.00
  raw.trading_rules.lot_size = Decimal{1, 3};     // 0.001
  raw.trading_rules.min_qty = Decimal{1, 3};      // 0.001
  raw.trading_rules.min_notional = Decimal{10, 0};
  raw.trading_rules.max_qty = Decimal{100, 0};

  raw.volume_24h = Decimal{1500000, 0};

  return raw;
}

NormalizationConfig MakeConfig() {
  NormalizationConfig cfg;
  cfg.fee_scale = 6;
  cfg.depth_config.max_levels_per_side = 20;
  return cfg;
}

// --- QuantizeDecimal tests ---

bool test_quantize_same_scale() {
  auto result = QuantizeDecimal(Decimal{12345, 4}, 4);
  return CheckDecimal(result, 12345, 4, "same scale");
}

bool test_quantize_reduce_scale() {
  // 0.12345 (scale 5) -> scale 3 -> 0.123 -> units=123
  auto result = QuantizeDecimal(Decimal{12345, 5}, 3);
  return CheckDecimal(result, 123, 3, "reduce scale");
}

bool test_quantize_increase_scale() {
  // 123 (scale 2, = 1.23) -> scale 4 -> units=12300
  auto result = QuantizeDecimal(Decimal{123, 2}, 4);
  return CheckDecimal(result, 12300, 4, "increase scale");
}

bool test_quantize_zero() {
  auto result = QuantizeDecimal(Decimal{0, 0}, 6);
  return CheckDecimal(result, 0, 6, "zero value");
}

bool test_quantize_negative_scale() {
  auto result = QuantizeDecimal(Decimal{100, 2}, -1);
  // target_scale clamped to 0: 100 at scale 2 -> scale 0 -> 100/100=1
  return CheckDecimal(result, 1, 0, "negative target scale");
}

// --- NormalizeSnapshot tests ---

bool test_normalize_basic_fields() {
  auto raw = MakeRaw();
  auto snap = NormalizeSnapshot(raw, MakeConfig());

  if (!Check(snap.venue_id() == "binance", "venue_id")) return false;
  if (!Check(snap.instrument().symbol() == "BTC/USDT", "symbol")) return false;
  if (!Check(snap.timestamp().seconds() == 1700000000, "timestamp")) return false;
  if (!Check(snap.status() == "connected", "status")) return false;
  if (!Check(!snap.meta().event_id().empty(), "event_id")) return false;
  if (!Check(snap.meta().source() == "venues", "source")) return false;
  return true;
}

bool test_normalize_bbo() {
  auto raw = MakeRaw();
  auto snap = NormalizeSnapshot(raw, MakeConfig());

  // best_bid = {7000000, 2}
  if (!CheckProtoDecimal(snap.best_bid(), 7000000, 2, "best_bid")) return false;
  // best_ask = {7001000, 2}
  if (!CheckProtoDecimal(snap.best_ask(), 7001000, 2, "best_ask")) return false;
  return true;
}

bool test_normalize_mid_spread() {
  auto raw = MakeRaw();
  auto snap = NormalizeSnapshot(raw, MakeConfig());

  // mid = (7000000 + 7001000) / 2 = 7000500 at scale 2
  if (!CheckProtoDecimal(snap.mid_price(), 7000500, 2, "mid_price")) return false;
  // spread = 7001000 - 7000000 = 1000 at scale 2 -> 10.00
  if (!CheckProtoDecimal(snap.spread(), 1000, 2, "spread")) return false;
  return true;
}

bool test_normalize_depth_canonicalized() {
  auto raw = MakeRaw();
  auto snap = NormalizeSnapshot(raw, MakeConfig());

  // Bids: 2 levels (70000.00, 69990.00), asks: 2 levels (70010.00, 70020.00)
  if (!Check(snap.bid_prices_size() == 2, "bid_prices_size")) return false;
  if (!Check(snap.bid_quantities_size() == 2, "bid_quantities_size")) return false;
  if (!Check(snap.ask_prices_size() == 2, "ask_prices_size")) return false;
  if (!Check(snap.ask_quantities_size() == 2, "ask_quantities_size")) return false;

  // Bids sorted desc
  auto bid0 = Decimal::from_proto(snap.bid_prices(0));
  auto bid1 = Decimal::from_proto(snap.bid_prices(1));
  if (!Check(static_cast<double>(bid0) > static_cast<double>(bid1),
             "bids sorted descending")) return false;

  // Asks sorted asc
  auto ask0 = Decimal::from_proto(snap.ask_prices(0));
  auto ask1 = Decimal::from_proto(snap.ask_prices(1));
  if (!Check(static_cast<double>(ask0) < static_cast<double>(ask1),
             "asks sorted ascending")) return false;

  return true;
}

bool test_normalize_fees() {
  auto raw = MakeRaw();
  auto snap = NormalizeSnapshot(raw, MakeConfig());

  // maker = {100, 6}, taker = {200, 6} -> already at scale 6
  if (!CheckProtoDecimal(snap.maker_fee(), 100, 6, "maker_fee")) return false;
  if (!CheckProtoDecimal(snap.taker_fee(), 200, 6, "taker_fee")) return false;
  return true;
}

bool test_normalize_fee_quantization() {
  auto raw = MakeRaw();
  // Fee at scale 8 -> should be quantized to scale 6
  raw.fees.maker = Decimal{10000, 8};  // 0.00010000
  raw.fees.taker = Decimal{20000, 8};  // 0.00020000

  auto snap = NormalizeSnapshot(raw, MakeConfig());

  // 10000 at scale 8 -> scale 6: 10000/100 = 100
  if (!CheckProtoDecimal(snap.maker_fee(), 100, 6, "quantized maker_fee")) return false;
  if (!CheckProtoDecimal(snap.taker_fee(), 200, 6, "quantized taker_fee")) return false;
  return true;
}

bool test_normalize_fee_upscale() {
  auto raw = MakeRaw();
  // Fee at scale 4 -> should be upscaled to scale 6
  raw.fees.maker = Decimal{1, 4};   // 0.0001
  raw.fees.taker = Decimal{2, 4};   // 0.0002

  auto snap = NormalizeSnapshot(raw, MakeConfig());

  // 1 at scale 4 -> scale 6: 1*100 = 100
  if (!CheckProtoDecimal(snap.maker_fee(), 100, 6, "upscaled maker_fee")) return false;
  if (!CheckProtoDecimal(snap.taker_fee(), 200, 6, "upscaled taker_fee")) return false;
  return true;
}

bool test_normalize_tick_lot() {
  auto raw = MakeRaw();
  auto snap = NormalizeSnapshot(raw, MakeConfig());

  if (!CheckProtoDecimal(snap.tick_size(), 100, 2, "tick_size")) return false;
  if (!CheckProtoDecimal(snap.lot_size(), 1, 3, "lot_size")) return false;
  return true;
}

bool test_normalize_volume() {
  auto raw = MakeRaw();
  auto snap = NormalizeSnapshot(raw, MakeConfig());

  if (!CheckProtoDecimal(snap.volume_24h(), 1500000, 0, "volume_24h")) return false;
  return true;
}

bool test_normalize_empty_book() {
  auto raw = MakeRaw();
  raw.bids.clear();
  raw.asks.clear();
  raw.best_bid = Decimal{0, 0};
  raw.best_ask = Decimal{0, 0};

  auto snap = NormalizeSnapshot(raw, MakeConfig());

  if (!Check(snap.bid_prices_size() == 0, "empty bids")) return false;
  if (!Check(snap.ask_prices_size() == 0, "empty asks")) return false;
  if (!CheckProtoDecimal(snap.mid_price(), 0, 0, "empty mid")) return false;
  if (!CheckProtoDecimal(snap.spread(), 0, 0, "empty spread")) return false;
  return true;
}

bool test_normalize_status_stale() {
  auto raw = MakeRaw();
  raw.status = VenueConnectionStatus::kStale;
  auto snap = NormalizeSnapshot(raw, MakeConfig());

  if (!Check(snap.status() == "stale", "stale status")) return false;
  return true;
}

bool test_normalize_status_disconnected() {
  auto raw = MakeRaw();
  raw.status = VenueConnectionStatus::kDisconnected;
  auto snap = NormalizeSnapshot(raw, MakeConfig());

  if (!Check(snap.status() == "disconnected", "disconnected status")) return false;
  return true;
}

bool test_normalize_depth_max_levels() {
  auto raw = MakeRaw();
  // Add many bid levels
  raw.bids.clear();
  for (int i = 0; i < 30; ++i) {
    raw.bids.push_back(VenueBookLevel{
        Decimal{7000000 - i * 100, 2}, Decimal{100, 3}});
  }
  raw.asks.clear();
  for (int i = 0; i < 30; ++i) {
    raw.asks.push_back(VenueBookLevel{
        Decimal{7001000 + i * 100, 2}, Decimal{100, 3}});
  }

  NormalizationConfig cfg = MakeConfig();
  cfg.depth_config.max_levels_per_side = 5;

  auto snap = NormalizeSnapshot(raw, cfg);

  if (!Check(snap.bid_prices_size() == 5, "max 5 bid levels")) return false;
  if (!Check(snap.ask_prices_size() == 5, "max 5 ask levels")) return false;
  return true;
}

bool test_normalize_partition_key() {
  auto raw = MakeRaw();
  auto snap = NormalizeSnapshot(raw, MakeConfig());

  if (!Check(snap.meta().partition_key() == "binance|BTC/USDT",
             "partition_key")) return false;
  return true;
}

bool test_normalize_config_overrides_depth() {
  auto raw = MakeRaw();

  NormalizationConfig cfg = MakeConfig();
  // Override tick/lot from config (not from trading_rules)
  cfg.depth_config.tick_size = Decimal{1000, 2};  // 10.00
  cfg.depth_config.lot_size = Decimal{100, 3};    // 0.100

  auto snap = NormalizeSnapshot(raw, cfg);

  // Book should still produce levels (tick=10 groups nearby prices)
  if (!Check(snap.bid_prices_size() > 0, "config override bids")) return false;
  if (!Check(snap.ask_prices_size() > 0, "config override asks")) return false;
  return true;
}

bool test_normalize_amm_pool_state_tags() {
  auto raw = MakeRaw();
  raw.venue_type = VenueType::kDex;

  cex::venues::domain::VenuePoolState pool;
  pool.pool_address = "0xpool";
  pool.sqrt_price_x96 = "79228162514264337593543950336";
  pool.tick = 120;
  pool.liquidity = "1500000000";
  pool.block_number = 777;
  pool.finalized = true;
  pool.ticks = {
      cex::venues::domain::VenuePoolTickLevel{-60, Decimal{5000, 0}},
      cex::venues::domain::VenuePoolTickLevel{0, Decimal{-2000, 0}},
      cex::venues::domain::VenuePoolTickLevel{60, Decimal{-3000, 0}},
  };
  raw.pool_state = pool;

  auto snap = NormalizeSnapshot(raw, MakeConfig());
  bool pass = true;
  pass = Check(snap.meta().tags().contains("venue_type"), "venue_type tag exists") && pass;
  pass = Check(snap.meta().tags().at("venue_type") == "dex", "venue_type=dex") && pass;
  pass = Check(snap.meta().tags().contains("amm.pool_state.present"),
               "amm.pool_state.present tag exists") && pass;
  pass = Check(snap.meta().tags().at("amm.pool_state.present") == "true",
               "amm.pool_state.present=true") && pass;
  pass = Check(snap.meta().tags().at("amm.pool_address") == "0xpool",
               "amm.pool_address") && pass;
  pass = Check(snap.meta().tags().at("amm.tick") == "120",
               "amm.tick serialized") && pass;
  pass = Check(!snap.meta().tags().at("amm.ticks").empty(),
               "amm.ticks serialized") && pass;
  return pass;
}

}  // namespace

int main() {
  bool all_passed = true;

  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) {
      std::cerr << "  in test: " << name << std::endl;
      all_passed = false;
    }
  };

  run("test_quantize_same_scale", test_quantize_same_scale);
  run("test_quantize_reduce_scale", test_quantize_reduce_scale);
  run("test_quantize_increase_scale", test_quantize_increase_scale);
  run("test_quantize_zero", test_quantize_zero);
  run("test_quantize_negative_scale", test_quantize_negative_scale);
  run("test_normalize_basic_fields", test_normalize_basic_fields);
  run("test_normalize_bbo", test_normalize_bbo);
  run("test_normalize_mid_spread", test_normalize_mid_spread);
  run("test_normalize_depth_canonicalized", test_normalize_depth_canonicalized);
  run("test_normalize_fees", test_normalize_fees);
  run("test_normalize_fee_quantization", test_normalize_fee_quantization);
  run("test_normalize_fee_upscale", test_normalize_fee_upscale);
  run("test_normalize_tick_lot", test_normalize_tick_lot);
  run("test_normalize_volume", test_normalize_volume);
  run("test_normalize_empty_book", test_normalize_empty_book);
  run("test_normalize_status_stale", test_normalize_status_stale);
  run("test_normalize_status_disconnected", test_normalize_status_disconnected);
  run("test_normalize_depth_max_levels", test_normalize_depth_max_levels);
  run("test_normalize_partition_key", test_normalize_partition_key);
  run("test_normalize_config_overrides_depth", test_normalize_config_overrides_depth);
  run("test_normalize_amm_pool_state_tags", test_normalize_amm_pool_state_tags);

  if (all_passed) {
    std::cout << "[OK] normalize_snapshot_test passed (21 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
