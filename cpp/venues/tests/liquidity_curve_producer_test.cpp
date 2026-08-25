#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "app/liquidity_curve_producer.hpp"
#include "cex/common/decimal.hpp"
#include "cex/common/proto.hpp"

namespace {

using cex::venues::app::IMessagePublisher;
using cex::venues::app::ISyntheticOrderRepository;
using cex::venues::app::LiquidityCurveProducer;
using cex::venues::app::LiquidityCurveProducerConfig;
using cex::venues::app::L3ImpactModel;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool IsFiniteNonNegative(const double value) {
  return std::isfinite(value) && value >= 0.0;
}

fob::common::v1::Decimal Dec(const int64_t units, const int32_t scale = 0) {
  fob::common::v1::Decimal out;
  out.set_units(units);
  out.set_scale(scale);
  return out;
}

double DecAsDouble(const fob::common::v1::Decimal& value) {
  double out = static_cast<double>(value.units());
  for (int32_t i = 0; i < value.scale(); ++i) {
    out /= 10.0;
  }
  return out;
}

std::string Tag(const fob::common::v1::EventMeta& meta, const std::string& key) {
  const auto it = meta.tags().find(key);
  if (it == meta.tags().end()) return {};
  return it->second;
}

double TagAsDouble(const fob::common::v1::EventMeta& meta,
                   const std::string& key,
                   const double fallback = 0.0) {
  const std::string text = Tag(meta, key);
  if (text.empty()) return fallback;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(value)) return fallback;
  return value;
}

struct FakePublisher final : public IMessagePublisher {
  struct Message {
    std::string topic;
    std::string key;
    std::string payload;
  };

  std::vector<Message> messages;

  bool Publish(const std::string& topic,
               const std::string& key,
               const std::string& payload) override {
    messages.push_back({topic, key, payload});
    return true;
  }
};

struct FakeSyntheticOrderRepository final : public ISyntheticOrderRepository {
  std::vector<fob::orders::v1::SyntheticFlowOrder> orders;
  bool fail{false};

  bool SaveSyntheticOrder(
      const fob::orders::v1::SyntheticFlowOrder& order) override {
    orders.push_back(order);
    return !fail;
  }
};

fob::venue::v1::VenueSnapshot MakeSnapshot() {
  fob::venue::v1::VenueSnapshot snap;
  snap.mutable_meta()->set_event_id("snapshot-1");
  snap.mutable_meta()->set_partition_key("binance|BTC/USDT");
  snap.set_venue_id("binance");
  snap.mutable_instrument()->set_symbol("BTC/USDT");
  snap.mutable_instrument()->set_base("BTC");
  snap.mutable_instrument()->set_quote("USDT");

  *snap.mutable_best_bid() = Dec(100, 0);
  *snap.mutable_best_ask() = Dec(101, 0);
  *snap.mutable_mid_price() = Dec(1005, 1);
  *snap.mutable_spread() = Dec(1, 0);
  *snap.mutable_tick_size() = Dec(1, 0);
  *snap.mutable_lot_size() = Dec(1, 0);

  *snap.add_bid_prices() = Dec(100, 0);
  *snap.add_bid_quantities() = Dec(2, 0);
  *snap.add_bid_prices() = Dec(99, 0);
  *snap.add_bid_quantities() = Dec(3, 0);

  *snap.add_ask_prices() = Dec(101, 0);
  *snap.add_ask_quantities() = Dec(2, 0);
  *snap.add_ask_prices() = Dec(102, 0);
  *snap.add_ask_quantities() = Dec(4, 0);

  return snap;
}

fob::venue::v1::VenueSnapshot MakeAmmTaggedSnapshot() {
  auto snap = MakeSnapshot();
  auto* tags = snap.mutable_meta()->mutable_tags();
  (*tags)["venue_type"] = "dex";
  (*tags)["amm.pool_state.present"] = "true";
  (*tags)["amm.pool_address"] = "0xpool";
  (*tags)["amm.sqrt_price_x96"] = "79228162514264337593543950336";
  (*tags)["amm.tick"] = "0";
  (*tags)["amm.liquidity"] = "1000000000";
  (*tags)["amm.block_number"] = "12345";
  (*tags)["amm.finalized"] = "true";
  (*tags)["amm.ticks"] = "-600,500000,0;-300,800000,0;0,-200000,0;300,-600000,0;600,-500000,0";
  return snap;
}

fob::venue::v1::VenueSnapshot MakeDustSnapshot() {
  fob::venue::v1::VenueSnapshot snap = MakeSnapshot();
  snap.clear_bid_prices();
  snap.clear_bid_quantities();
  snap.clear_ask_prices();
  snap.clear_ask_quantities();
  *snap.mutable_lot_size() = Dec(1, 2);

  *snap.add_bid_prices() = Dec(100, 0);
  *snap.add_bid_quantities() = Dec(1, 2);
  *snap.add_bid_prices() = Dec(99, 0);
  *snap.add_bid_quantities() = Dec(5, 2);
  *snap.add_bid_prices() = Dec(98, 0);
  *snap.add_bid_quantities() = Dec(3, 1);
  *snap.add_bid_prices() = Dec(97, 0);
  *snap.add_bid_quantities() = Dec(7, 1);

  *snap.add_ask_prices() = Dec(101, 0);
  *snap.add_ask_quantities() = Dec(1, 2);
  *snap.add_ask_prices() = Dec(102, 0);
  *snap.add_ask_quantities() = Dec(5, 2);
  *snap.add_ask_prices() = Dec(103, 0);
  *snap.add_ask_quantities() = Dec(3, 1);
  *snap.add_ask_prices() = Dec(104, 0);
  *snap.add_ask_quantities() = Dec(7, 1);

  return snap;
}

fob::venue::v1::VenueSnapshot MakeHighPriceSnapshot() {
  fob::venue::v1::VenueSnapshot snap;
  snap.mutable_meta()->set_event_id("snapshot-high-price");
  snap.mutable_meta()->set_partition_key("binance|BTC/USDT");
  snap.set_venue_id("binance");
  snap.mutable_instrument()->set_symbol("BTC/USDT");
  snap.mutable_instrument()->set_base("BTC");
  snap.mutable_instrument()->set_quote("USDT");

  *snap.mutable_best_bid() = Dec(6843510, 2);
  *snap.mutable_best_ask() = Dec(6843650, 2);
  *snap.mutable_mid_price() = Dec(6843580, 2);
  *snap.mutable_spread() = Dec(140, 2);
  *snap.mutable_tick_size() = Dec(10, 2);
  *snap.mutable_lot_size() = Dec(1, 4);
  *snap.mutable_taker_fee() = Dec(10, 4);

  *snap.add_bid_prices() = Dec(6843510, 2);
  *snap.add_bid_quantities() = Dec(4200, 3);
  *snap.add_bid_prices() = Dec(6843410, 2);
  *snap.add_bid_quantities() = Dec(4010, 3);
  *snap.add_bid_prices() = Dec(6843310, 2);
  *snap.add_bid_quantities() = Dec(3790, 3);

  *snap.add_ask_prices() = Dec(6843650, 2);
  *snap.add_ask_quantities() = Dec(3970, 3);
  *snap.add_ask_prices() = Dec(6843750, 2);
  *snap.add_ask_quantities() = Dec(3810, 3);
  *snap.add_ask_prices() = Dec(6843850, 2);
  *snap.add_ask_quantities() = Dec(3620, 3);

  return snap;
}

fob::venue::v1::VenueSnapshot MakeLiveLikeCexSnapshot() {
  fob::venue::v1::VenueSnapshot snap;
  snap.mutable_meta()->set_event_id("snapshot-live-like");
  snap.mutable_meta()->set_partition_key("binance|BTC/USDT");
  snap.set_venue_id("binance");
  snap.mutable_instrument()->set_symbol("BTC/USDT");
  snap.mutable_instrument()->set_base("BTC");
  snap.mutable_instrument()->set_quote("USDT");

  *snap.mutable_best_bid() = Dec(7768318000000LL, 8);
  *snap.mutable_best_ask() = Dec(7768319000000LL, 8);
  *snap.mutable_mid_price() = Dec(7768318500000LL, 8);
  *snap.mutable_spread() = Dec(1000000LL, 8);
  *snap.mutable_tick_size() = Dec(1000000LL, 8);
  *snap.mutable_lot_size() = Dec(1, 8);
  *snap.mutable_taker_fee() = Dec(1, 3);   // 0.1%

  *snap.add_bid_prices() = Dec(7768318000000LL, 8);
  *snap.add_bid_quantities() = Dec(435964000LL, 8);
  *snap.add_bid_prices() = Dec(7768317000000LL, 8);
  *snap.add_bid_quantities() = Dec(238000LL, 8);
  *snap.add_bid_prices() = Dec(7768228000000LL, 8);
  *snap.add_bid_quantities() = Dec(120000LL, 8);
  *snap.add_bid_prices() = Dec(7768227000000LL, 8);
  *snap.add_bid_quantities() = Dec(1126000LL, 8);

  *snap.add_ask_prices() = Dec(7768319000000LL, 8);
  *snap.add_ask_quantities() = Dec(73235000LL, 8);
  *snap.add_ask_prices() = Dec(7768320000000LL, 8);
  *snap.add_ask_quantities() = Dec(49000LL, 8);
  *snap.add_ask_prices() = Dec(7768330000000LL, 8);
  *snap.add_ask_quantities() = Dec(7000LL, 8);
  *snap.add_ask_prices() = Dec(7768400000000LL, 8);
  *snap.add_ask_quantities() = Dec(120000LL, 8);

  return snap;
}

fob::venue::v1::VenueSnapshot MakeCapturedBinanceSnapshot() {
  fob::venue::v1::VenueSnapshot snap;
  snap.mutable_meta()->set_event_id("snapshot-captured-binance");
  snap.mutable_meta()->set_partition_key("binance|BTC/USDT");
  snap.set_venue_id("binance");
  snap.mutable_instrument()->set_symbol("BTC/USDT");
  snap.mutable_instrument()->set_base("BTC");
  snap.mutable_instrument()->set_quote("USDT");

  *snap.mutable_best_bid() = Dec(776549, 1);
  *snap.mutable_best_ask() = Dec(776549, 1);
  *snap.mutable_mid_price() = Dec(776549, 1);
  *snap.mutable_spread() = Dec(0, 1);
  *snap.mutable_tick_size() = Dec(1, 1);
  *snap.mutable_lot_size() = Dec(1, 3);
  *snap.mutable_taker_fee() = Dec(1, 3);

  *snap.add_bid_prices() = Dec(776549, 1);
  *snap.add_bid_quantities() = Dec(7149, 3);
  *snap.add_bid_prices() = Dec(776549, 1);
  *snap.add_bid_quantities() = Dec(515, 3);
  *snap.add_bid_prices() = Dec(776529, 1);
  *snap.add_bid_quantities() = Dec(4, 3);
  *snap.add_bid_prices() = Dec(776509, 1);
  *snap.add_bid_quantities() = Dec(32, 3);
  *snap.add_bid_prices() = Dec(776509, 1);
  *snap.add_bid_quantities() = Dec(109, 3);
  *snap.add_bid_prices() = Dec(776504, 1);
  *snap.add_bid_quantities() = Dec(690, 3);
  *snap.add_bid_prices() = Dec(776504, 1);
  *snap.add_bid_quantities() = Dec(387, 3);
  *snap.add_bid_prices() = Dec(776492, 1);
  *snap.add_bid_quantities() = Dec(527, 3);
  *snap.add_bid_prices() = Dec(776490, 1);
  *snap.add_bid_quantities() = Dec(3, 3);

  *snap.add_ask_prices() = Dec(776549, 1);
  *snap.add_ask_quantities() = Dec(1850, 3);
  *snap.add_ask_prices() = Dec(776551, 1);
  *snap.add_ask_quantities() = Dec(60, 3);
  *snap.add_ask_prices() = Dec(776558, 1);
  *snap.add_ask_quantities() = Dec(6, 3);
  *snap.add_ask_prices() = Dec(776559, 1);
  *snap.add_ask_quantities() = Dec(69, 3);
  *snap.add_ask_prices() = Dec(776560, 1);
  *snap.add_ask_quantities() = Dec(10, 3);
  *snap.add_ask_prices() = Dec(776568, 1);
  *snap.add_ask_quantities() = Dec(3, 3);
  *snap.add_ask_prices() = Dec(776569, 1);
  *snap.add_ask_quantities() = Dec(48, 3);
  *snap.add_ask_prices() = Dec(776577, 1);
  *snap.add_ask_quantities() = Dec(1, 3);

  return snap;
}

fob::execution::v1::ExecutionIntent MakeIntent(const fob::common::v1::Side side,
                                               const int64_t qty_units,
                                               const int32_t qty_scale = 0) {
  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id("intent-1");
  intent.set_venue("binance");
  intent.mutable_instrument()->set_symbol("BTC/USDT");
  intent.mutable_instrument()->set_base("BTC");
  intent.mutable_instrument()->set_quote("USDT");
  intent.set_side(side);
  *intent.mutable_target_qty() = Dec(qty_units, qty_scale);
  return intent;
}

fob::execution::v1::ExecutionReport MakeReport(const int64_t filled_qty_units,
                                               const int64_t avg_price_units,
                                               const int32_t qty_scale = 0,
                                               const int32_t price_scale = 0) {
  fob::execution::v1::ExecutionReport report;
  report.set_venue("binance");
  report.mutable_instrument()->set_symbol("BTC/USDT");
  report.mutable_instrument()->set_base("BTC");
  report.mutable_instrument()->set_quote("USDT");
  report.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED);
  *report.mutable_filled_qty() = Dec(filled_qty_units, qty_scale);
  *report.mutable_average_price() = Dec(avg_price_units, price_scale);
  return report;
}

fob::execution::v1::ExecutionReport MakeRejectedReport() {
  fob::execution::v1::ExecutionReport report;
  report.set_venue("binance");
  report.mutable_instrument()->set_symbol("BTC/USDT");
  report.mutable_instrument()->set_base("BTC");
  report.mutable_instrument()->set_quote("USDT");
  report.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED);
  report.mutable_error()->set_code("VENUE_REJECTED");
  report.mutable_error()->set_message("venue rejected child order");
  return report;
}

bool TestPublishesRegularizedCurve() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.tau_ms = 1000.0;
  cfg.apply_convexification = true;
  cfg.apply_moreau_l2 = true;
  cfg.apply_tikhonov_l2 = true;
  cfg.apply_fenchel_legendre = true;
  cfg.degradation.min_l2_confidence = 0.0;

  LiquidityCurveProducer producer(&publisher, cfg);

  const bool ok = producer.Publish(MakeSnapshot());
  if (!Check(ok, "Publish should succeed")) return false;
  if (!Check(publisher.messages.size() == 1, "Exactly one Kafka message expected")) return false;
  if (!Check(publisher.messages[0].topic == "venue.liquidity.fob", "Topic mismatch")) return false;
  if (!Check(publisher.messages[0].key == "binance|BTC/USDT", "Partition key mismatch")) return false;

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages[0].payload, curve),
             "Curve payload must be parseable")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.venue_id() == "binance", "venue_id must be copied") && pass;
  pass = Check(curve.instrument().symbol() == "BTC/USDT", "symbol must be copied") && pass;
  pass = Check(curve.curve_id() == curve.meta().event_id(),
               "curve_id must be linked to curve event id") && pass;
  pass = Check(curve.level() == "L2", "level must be L2") && pass;
  pass = Check(curve.has_bid_curve() && curve.has_ask_curve(), "both side curves must exist") && pass;
  pass = Check(curve.bid_curve().q_grid_size() > 0, "bid q_grid must be non-empty") && pass;
  pass = Check(curve.ask_curve().q_grid_size() > 0, "ask q_grid must be non-empty") && pass;
  pass = Check(curve.bid_curve().p_of_q_size() == curve.bid_curve().q_grid_size(),
               "bid p_of_q/q_grid sizes must match") && pass;
  pass = Check(curve.ask_curve().p_of_q_size() == curve.ask_curve().q_grid_size(),
               "ask p_of_q/q_grid sizes must match") && pass;
  pass = Check(curve.bid_curve().s_star_of_p_size() > 0, "bid dual S*(p) must be present") && pass;
  pass = Check(curve.ask_curve().s_star_of_p_size() > 0, "ask dual S*(p) must be present") && pass;
  pass = Check(curve.bid_curve().q_star_of_p_size() == curve.bid_curve().s_star_of_p_size(),
               "bid dual table sizes must match") && pass;
  pass = Check(curve.ask_curve().q_star_of_p_size() == curve.ask_curve().s_star_of_p_size(),
               "ask dual table sizes must match") && pass;
  pass = Check(curve.bid_curve().l_of_v_monotone_size() > 0,
               "bid l_of_v_monotone must be present") && pass;
  pass = Check(curve.ask_curve().l_of_v_monotone_size() > 0,
               "ask l_of_v_monotone must be present") && pass;
  pass = Check(curve.confidence() >= 0.0 && curve.confidence() <= 1.0,
               "confidence must be in [0,1]") && pass;
  pass = Check(curve.tau_ms() == 1000.0, "tau_ms must be copied from config") && pass;
  pass = Check(curve.schema_version() == 1,
               "default schema_version must be 1") && pass;
  pass = Check(curve.min_compatible_schema_version() == 1,
               "default min compatible schema version must be 1") && pass;
  pass = Check(curve.producer_version() == "f11-curve-15",
               "default producer_version must identify F11-CURVE-15 producer") && pass;
  pass = Check(Tag(curve.meta(), "payload_type") == "fob.venue.v1.VenueLiquidityCurve",
               "payload_type tag must identify direct protobuf payload") && pass;
  pass = Check(Tag(curve.meta(), "payload_format") == "protobuf",
               "payload_format tag must be protobuf") && pass;
  pass = Check(Tag(curve.meta(), "schema_version") == "1",
               "schema_version tag must match payload field") && pass;
  return pass;
}

bool TestPublishesL2ForHighPriceSnapshot() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.tau_ms = 1000.0;
  cfg.apply_convexification = true;
  cfg.apply_moreau_l2 = true;
  cfg.apply_tikhonov_l2 = true;
  cfg.apply_fenchel_legendre = true;

  LiquidityCurveProducer producer(&publisher, cfg);

  const bool ok = producer.Publish(MakeHighPriceSnapshot());
  if (!Check(ok, "High-price publish should succeed")) return false;
  if (!Check(publisher.messages.size() == 1, "High-price publish must emit one message")) {
    return false;
  }

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages[0].payload, curve),
             "High-price curve payload must be parseable")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.level() == "L2", "High-price snapshot must retain L2") && pass;
  pass = Check(curve.confidence() >= 0.25, "High-price L2 confidence must pass threshold") && pass;
  pass = Check(curve.epsilon1() < 25.0, "High-price L2 epsilon1 must stay bounded") && pass;
  return pass;
}

bool TestLiveLikeHighScaleSnapshotRetainsL2() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.level = "L2";

  LiquidityCurveProducer producer(&publisher, cfg);

  const bool ok = producer.Publish(MakeLiveLikeCexSnapshot());
  if (!Check(ok, "Live-like high-scale publish should succeed")) return false;
  if (!Check(publisher.messages.size() == 1, "Live-like high-scale publish must emit one message")) {
    return false;
  }

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages[0].payload, curve),
             "Live-like high-scale curve payload must be parseable")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.level() == "L2", "Live-like high-scale snapshot must retain L2") && pass;
  pass = Check(curve.confidence() >= 0.1, "Live-like high-scale L2 confidence must pass threshold") && pass;
  pass = Check(curve.epsilon1() < 25.0, "Live-like high-scale L2 epsilon1 must stay bounded") && pass;
  pass = Check(curve.epsilon2() < 5.0, "Live-like high-scale L2 epsilon2 must stay in green zone") && pass;
  return pass;
}

bool TestLiveLikeHighScaleSnapshotCanBuildL3() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.level = "L3";
  cfg.l3_impact.enabled = true;

  LiquidityCurveProducer producer(&publisher, cfg);

  const bool ok = producer.Publish(MakeLiveLikeCexSnapshot());
  if (!Check(ok, "Live-like high-scale L3 publish should succeed")) return false;
  if (!Check(publisher.messages.size() == 1, "Live-like high-scale L3 publish must emit one message")) {
    return false;
  }

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages[0].payload, curve),
             "Live-like high-scale L3 curve payload must be parseable")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.level() == "L3", "Live-like high-scale snapshot must retain L3") && pass;
  pass = Check(curve.confidence() >= 0.35, "Live-like high-scale L3 confidence must pass threshold") && pass;
  pass = Check(curve.epsilon1() < 25.0, "Live-like high-scale L3 epsilon1 must stay bounded") && pass;
  pass = Check(curve.epsilon2() < 5.0, "Live-like high-scale L3 epsilon2 must stay in green zone") && pass;
  return pass;
}

bool TestLiveLikeL3CalibrationDoesNotSaturateCostLayer() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.level = "L3";
  cfg.l3_impact.enabled = true;
  cfg.l3_impact.model = L3ImpactModel::kLinear;
  cfg.l3_impact.max_history = 16;
  cfg.l3_impact.max_relative_impact = 0.5;
  cfg.l3_impact.execution_blend_weight = 1.0;
  cfg.l3_impact.min_samples_for_full_weight = 1;
  cfg.degradation.epsilon1_green_bps = 1.0e6;
  cfg.degradation.epsilon2_green_bps = 1.0e6;
  cfg.degradation.epsilon3_green_bps = 1.0e6;
  cfg.degradation.min_l3_confidence = 0.0;

  LiquidityCurveProducer producer(&publisher, cfg);

  const bool base_ok = producer.Publish(MakeLiveLikeCexSnapshot());
  if (!Check(base_ok, "Live-like L3 baseline publish should succeed")) return false;

  const auto buy_intent = MakeIntent(fob::common::v1::SIDE_BUY, 5000000LL, 8);
  producer.ObserveExecution(buy_intent, MakeReport(5000000LL, 7769000000000LL, 8, 8));
  producer.ObserveExecution(buy_intent, MakeReport(5000000LL, 7770000000000LL, 8, 8));

  const bool calibrated_ok = producer.Publish(MakeLiveLikeCexSnapshot());
  if (!Check(calibrated_ok, "Live-like L3 calibrated publish should succeed")) return false;
  if (!Check(!publisher.messages.empty(), "Live-like L3 calibrated message must exist")) return false;

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages.back().payload, curve),
             "Live-like L3 calibrated payload parse")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.has_ask_curve(), "Live-like L3 calibrated curve must contain ask side") && pass;
  if (!curve.has_ask_curve()) {
    return pass;
  }

  const auto& ask_curve = curve.ask_curve();
  pass = Check(ask_curve.q_grid_size() == ask_curve.s_of_q_size(),
               "ask q_grid and s_of_q sizes must match") && pass;
  if (ask_curve.q_grid_size() == 0 || ask_curve.q_grid_size() != ask_curve.s_of_q_size()) {
    return pass;
  }

  for (int i = 1; i < ask_curve.s_of_q_size(); ++i) {
    pass = Check(ask_curve.s_of_q(i) >= ask_curve.s_of_q(i - 1),
                 "ask cumulative cost must remain non-decreasing after L3 calibration") && pass;
  }

  const double last_q = ask_curve.q_grid(ask_curve.q_grid_size() - 1);
  const double last_cost = ask_curve.s_of_q(ask_curve.s_of_q_size() - 1);
  const double min_expected_cost = last_q * 1000.0;
  pass = Check(last_cost > min_expected_cost,
               "ask cumulative cost must stay in realistic quote-units range") && pass;
  pass = Check(last_cost > 1000.0,
               "ask cumulative cost must not saturate near fixed-point clamp") && pass;
  return pass;
}

bool TestCapturedBinanceSnapshotRetainsL2() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.level = "L2";

  LiquidityCurveProducer producer(&publisher, cfg);

  const bool ok = producer.Publish(MakeCapturedBinanceSnapshot());
  if (!Check(ok, "Captured binance publish should succeed")) return false;
  if (!Check(publisher.messages.size() == 1, "Captured binance publish must emit one message")) {
    return false;
  }

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages[0].payload, curve),
             "Captured binance curve payload must be parseable")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.level() == "L2", "Captured binance snapshot must retain L2") && pass;
  pass = Check(curve.confidence() >= 0.1, "Captured binance L2 confidence must pass threshold") && pass;
  pass = Check(curve.epsilon1() < 25.0, "Captured binance L2 epsilon1 must stay bounded") && pass;
  pass = Check(curve.epsilon2() < 5.0, "Captured binance L2 epsilon2 must stay in green zone") && pass;
  return pass;
}

bool TestAmmDirectPipelineBuildsCurveAndPublishesComparison() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.level = "L2";
  cfg.tau_ms = 1000.0;
  cfg.degradation.min_l2_confidence = 0.0;
  cfg.amm_direct.enabled = true;
  cfg.amm_direct.compare_with_virtual_lob = true;
  cfg.amm_direct.max_segments_per_side = 8;

  LiquidityCurveProducer producer(&publisher, cfg);
  const bool ok = producer.Publish(MakeAmmTaggedSnapshot());
  if (!Check(ok, "direct AMM publish must succeed")) return false;
  if (!Check(publisher.messages.size() == 1, "one direct AMM message expected")) return false;

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages[0].payload, curve),
             "direct AMM curve payload parse")) {
    return false;
  }

  bool pass = true;
  pass = Check(Tag(curve.meta(), "amm_path") == "direct",
               "direct AMM snapshot must use direct path") && pass;
  pass = Check(Tag(curve.meta(), "amm_direct_enabled") == "true",
               "amm_direct_enabled tag must be true") && pass;
  pass = Check(!Tag(curve.meta(), "calibration.amm.pool_fee_rate").empty(),
               "AMM calibration pool fee tag must exist") && pass;
  pass = Check(!Tag(curve.meta(), "calibration.amm.gas_cost_quote").empty(),
               "AMM calibration gas tag must exist") && pass;
  pass = Check(!Tag(curve.meta(), "calibration.amm.execution_overhead_bps").empty(),
               "AMM calibration overhead tag must exist") && pass;
  pass = Check(!Tag(curve.meta(), "calibration.amm.slippage_multiplier").empty(),
               "AMM calibration slippage tag must exist") && pass;
  pass = Check(!Tag(curve.meta(), "calibration.amm.effective_fee_rate").empty(),
               "AMM calibration effective fee tag must exist") && pass;
  pass = Check(!Tag(curve.meta(), "amm_direct_vs_virtual_price_bps").empty(),
               "direct-vs-virtual price comparison tag must exist") && pass;
  pass = Check(!Tag(curve.meta(), "amm_direct_vs_virtual_cost_rel").empty(),
               "direct-vs-virtual cost comparison tag must exist") && pass;
  pass = Check(curve.bid_curve().q_grid_size() > 1,
               "direct AMM bid curve must have depth points") && pass;
  pass = Check(curve.ask_curve().q_grid_size() > 1,
               "direct AMM ask curve must have depth points") && pass;
  return pass;
}

bool TestAmmDirectCorrectionsAdjustCurveAndPublishCalibrationMeta() {
  auto snapshot = MakeAmmTaggedSnapshot();
  *snapshot.mutable_taker_fee() = Dec(3, 3);  // 0.003, fallback pool fee source.

  LiquidityCurveProducerConfig base_cfg;
  base_cfg.topic = "venue.liquidity.fob";
  base_cfg.level = "L2";
  base_cfg.tau_ms = 1000.0;
  base_cfg.degradation.min_l2_confidence = 0.0;
  base_cfg.amm_direct.enabled = true;
  base_cfg.amm_direct.compare_with_virtual_lob = true;
  base_cfg.amm_direct.max_segments_per_side = 8;
  base_cfg.amm_direct.pool_fee_rate_override = 0.0;
  base_cfg.amm_direct.gas_cost_quote = 0.0;
  base_cfg.amm_direct.execution_overhead_bps = 0.0;
  base_cfg.amm_direct.slippage_multiplier = 1.0;

  FakePublisher base_publisher;
  LiquidityCurveProducer base_producer(&base_publisher, base_cfg);
  const bool base_ok = base_producer.Publish(snapshot);
  if (!Check(base_ok, "baseline AMM direct publish must succeed")) return false;

  fob::venue::v1::VenueLiquidityCurve base_curve;
  if (!Check(cex::common::from_bytes(base_publisher.messages.back().payload, base_curve),
             "baseline AMM curve parse")) {
    return false;
  }

  LiquidityCurveProducerConfig corrected_cfg = base_cfg;
  corrected_cfg.amm_direct.pool_fee_rate_override = 0.003;
  corrected_cfg.amm_direct.gas_cost_quote = 500.0;
  corrected_cfg.amm_direct.execution_overhead_bps = 25.0;
  corrected_cfg.amm_direct.slippage_multiplier = 1.5;

  FakePublisher corrected_publisher;
  LiquidityCurveProducer corrected_producer(&corrected_publisher, corrected_cfg);
  const bool corrected_ok = corrected_producer.Publish(snapshot);
  if (!Check(corrected_ok, "corrected AMM direct publish must succeed")) return false;

  fob::venue::v1::VenueLiquidityCurve corrected_curve;
  if (!Check(cex::common::from_bytes(corrected_publisher.messages.back().payload, corrected_curve),
             "corrected AMM curve parse")) {
    return false;
  }

  bool pass = true;
  pass = Check(corrected_curve.ask_curve().p_of_q(0) > base_curve.ask_curve().p_of_q(0),
               "pool fee + overhead must increase ask executable price") && pass;
  pass = Check(corrected_curve.bid_curve().p_of_q(0) < base_curve.bid_curve().p_of_q(0),
               "pool fee + overhead must decrease bid executable price") && pass;
  pass = Check(corrected_curve.ask_curve().s_of_q(1) > base_curve.ask_curve().s_of_q(1),
               "gas/impact corrections must increase ask cumulative cost") && pass;
  pass = Check(corrected_curve.bid_curve().s_of_q(1) < base_curve.bid_curve().s_of_q(1),
               "gas/impact corrections must reduce bid net proceeds layer") && pass;

  pass = Check(std::fabs(TagAsDouble(corrected_curve.meta(), "calibration.amm.pool_fee_rate") -
                         0.003) < 1e-12,
               "calibration pool fee tag must match config") && pass;
  pass = Check(std::fabs(TagAsDouble(corrected_curve.meta(), "calibration.amm.gas_cost_quote") -
                         500.0) < 1e-9,
               "calibration gas tag must match config") && pass;
  pass = Check(std::fabs(
                   TagAsDouble(corrected_curve.meta(),
                               "calibration.amm.execution_overhead_bps") - 25.0) < 1e-9,
               "calibration overhead bps tag must match config") && pass;
  pass = Check(std::fabs(
                   TagAsDouble(corrected_curve.meta(),
                               "calibration.amm.slippage_multiplier") - 1.5) < 1e-12,
               "calibration slippage multiplier tag must match config") && pass;
  pass = Check(TagAsDouble(corrected_curve.meta(), "calibration.amm.effective_fee_rate") >
                   TagAsDouble(corrected_curve.meta(), "calibration.amm.pool_fee_rate"),
               "effective fee tag must include overhead component") && pass;
  return pass;
}

bool TestLiquidityFobVersioningMetadataIsConfigurable() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.level = "L2";
  cfg.tau_ms = 1000.0;
  cfg.degradation.min_l2_confidence = 0.0;
  cfg.liquidity_fob_versioning.schema_version = 3;
  cfg.liquidity_fob_versioning.min_compatible_schema_version = 1;
  cfg.liquidity_fob_versioning.producer_version = "venues-test-producer";
  cfg.liquidity_fob_versioning.model_config_version = "curve-config-v7";

  LiquidityCurveProducer producer(&publisher, cfg);
  const bool ok = producer.Publish(MakeSnapshot());
  if (!Check(ok, "versioned venue.liquidity.fob publish must succeed")) return false;
  if (!Check(publisher.messages.size() == 1,
             "versioned publish must produce one Kafka record")) {
    return false;
  }
  if (!Check(publisher.messages[0].topic == "venue.liquidity.fob",
             "versioned publish topic mismatch")) {
    return false;
  }

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages[0].payload, curve),
             "versioned payload must remain a direct VenueLiquidityCurve protobuf")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.schema_version() == 3,
               "configured schema_version must be published") && pass;
  pass = Check(curve.min_compatible_schema_version() == 1,
               "configured min compatible schema version must be published") && pass;
  pass = Check(curve.producer_version() == "venues-test-producer",
               "configured producer_version must be published") && pass;
  pass = Check(Tag(curve.meta(), "topic") == "venue.liquidity.fob",
               "topic tag must match Kafka topic") && pass;
  pass = Check(Tag(curve.meta(), "schema_version") == "3",
               "schema_version tag must match configured field") && pass;
  pass = Check(Tag(curve.meta(), "min_compatible_schema_version") == "1",
               "min compatible schema tag must match configured field") && pass;
  pass = Check(Tag(curve.meta(), "producer_version") == "venues-test-producer",
               "producer_version tag must match configured field") && pass;
  pass = Check(Tag(curve.meta(), "model_config_version") == "curve-config-v7",
               "model config version tag must be published") && pass;
  pass = Check(Tag(curve.meta(), "requested_level") == "L2",
               "requested level tag must be published") && pass;
  pass = Check(Tag(curve.meta(), "effective_level") == "L2",
               "effective level tag must be published") && pass;
  pass = Check(!Tag(curve.meta(), "build_latency_ms").empty(),
               "build latency tag must be published for p95 monitoring") && pass;
  return pass;
}

bool TestTakerFeeAdjustsEffectiveCurves() {
  LiquidityCurveProducerConfig cfg;
  cfg.level = "L1";
  cfg.apply_convexification = false;
  cfg.apply_moreau_l2 = false;
  cfg.apply_tikhonov_l2 = false;
  cfg.apply_fenchel_legendre = false;
  cfg.degradation.min_l1_confidence = 0.0;

  FakePublisher no_fee_publisher;
  LiquidityCurveProducer no_fee_producer(&no_fee_publisher, cfg);
  const bool no_fee_ok = no_fee_producer.Publish(MakeSnapshot());
  if (!Check(no_fee_ok, "no-fee L1 publish must succeed")) return false;

  fob::venue::v1::VenueLiquidityCurve no_fee_curve;
  if (!Check(cex::common::from_bytes(no_fee_publisher.messages.back().payload,
                                     no_fee_curve),
             "no-fee L1 payload parse")) {
    return false;
  }

  FakePublisher fee_publisher;
  LiquidityCurveProducer fee_producer(&fee_publisher, cfg);
  auto fee_snapshot = MakeSnapshot();
  *fee_snapshot.mutable_taker_fee() = Dec(1, 3);  // 0.1%
  const bool fee_ok = fee_producer.Publish(fee_snapshot);
  if (!Check(fee_ok, "fee-adjusted L1 publish must succeed")) return false;

  fob::venue::v1::VenueLiquidityCurve fee_curve;
  if (!Check(cex::common::from_bytes(fee_publisher.messages.back().payload,
                                     fee_curve),
             "fee-adjusted L1 payload parse")) {
    return false;
  }

  bool pass = true;
  pass = Check(std::fabs(fee_curve.ask_curve().p_of_q(0) -
                         no_fee_curve.ask_curve().p_of_q(0) * 1.001) < 1e-6,
               "taker fee must increase ask/buy effective price") && pass;
  pass = Check(std::fabs(fee_curve.bid_curve().p_of_q(0) -
                         no_fee_curve.bid_curve().p_of_q(0) * 0.999) < 1e-6,
               "taker fee must decrease bid/sell effective price") && pass;
  pass = Check(fee_curve.ask_curve().s_of_q(1) >
                   no_fee_curve.ask_curve().s_of_q(1),
               "taker fee must increase ask/buy cost layer") && pass;
  pass = Check(fee_curve.bid_curve().s_of_q(1) <
                   no_fee_curve.bid_curve().s_of_q(1),
               "taker fee must decrease bid/sell proceeds layer") && pass;
  pass = Check(Tag(fee_curve.meta(), "apply_taker_fee") == "true",
               "fee application tag must be true") && pass;
  pass = Check(Tag(fee_curve.meta(), "taker_fee") != "0.000000",
               "taker fee tag must be non-zero") && pass;
  return pass;
}

bool TestMinQtyFiltersDustLevels() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.level = "L1";
  cfg.apply_convexification = false;
  cfg.apply_moreau_l2 = false;
  cfg.apply_tikhonov_l2 = false;
  cfg.apply_fenchel_legendre = false;
  cfg.input.min_qty = cex::common::Decimal{1, 1};  // 0.1
  cfg.degradation.min_l1_confidence = 0.0;

  LiquidityCurveProducer producer(&publisher, cfg);
  const bool ok = producer.Publish(MakeDustSnapshot());
  if (!Check(ok, "dust-filtered L1 publish must succeed")) return false;

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages.back().payload, curve),
             "dust-filtered payload parse")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.bid_curve().p_of_q(0) == 98.0,
               "bid dust below min_qty must be removed before p(q)") && pass;
  pass = Check(curve.ask_curve().p_of_q(0) == 103.0,
               "ask dust below min_qty must be removed before p(q)") && pass;
  pass = Check(curve.bid_curve().q_grid(curve.bid_curve().q_grid_size() - 1) == 1.0,
               "bid q_grid must contain only non-dust quantity") && pass;
  pass = Check(curve.ask_curve().q_grid(curve.ask_curve().q_grid_size() - 1) == 1.0,
               "ask q_grid must contain only non-dust quantity") && pass;
  pass = Check(Tag(curve.meta(), "min_qty") == "0.1",
               "min_qty tag must be published") && pass;
  return pass;
}

bool TestL1L2L3LevelsAndQualityMetrics() {
  auto publish_curve = [](const LiquidityCurveProducerConfig& cfg,
                          fob::venue::v1::VenueLiquidityCurve* out) {
    FakePublisher publisher;
    LiquidityCurveProducer producer(&publisher, cfg);
    const bool ok = producer.Publish(MakeSnapshot());
    if (!ok || publisher.messages.empty()) return false;
    return cex::common::from_bytes(publisher.messages.back().payload, *out);
  };

  bool pass = true;

  {
    LiquidityCurveProducerConfig l1_cfg;
    l1_cfg.level = "L1";
    l1_cfg.degradation.min_l1_confidence = 0.0;

    fob::venue::v1::VenueLiquidityCurve l1;
    pass = Check(publish_curve(l1_cfg, &l1), "L1 publish + parse must succeed") && pass;
    if (l1.has_bid_curve() && l1.bid_curve().p_of_q_size() >= 2) {
      pass = Check(l1.bid_curve().p_of_q(0) >= l1.bid_curve().p_of_q(1),
                   "L1 bid p(q) must be monotone non-increasing") && pass;
    }
    if (l1.has_ask_curve() && l1.ask_curve().p_of_q_size() >= 2) {
      pass = Check(l1.ask_curve().p_of_q(0) <= l1.ask_curve().p_of_q(1),
                   "L1 ask p(q) must be monotone non-decreasing") && pass;
    }
    pass = Check(l1.level() == "L1", "L1 level must stay L1") && pass;
    pass = Check(IsFiniteNonNegative(l1.epsilon1()), "L1 epsilon1 must be finite >= 0") && pass;
    pass = Check(IsFiniteNonNegative(l1.epsilon2()), "L1 epsilon2 must be finite >= 0") && pass;
    pass = Check(IsFiniteNonNegative(l1.epsilon3()), "L1 epsilon3 must be finite >= 0") && pass;
    pass = Check(l1.confidence() >= 0.0 && l1.confidence() <= 1.0,
                 "L1 confidence must be in [0,1]") && pass;
  }

  {
    LiquidityCurveProducerConfig l2_cfg;
    l2_cfg.level = "L2";
    l2_cfg.degradation.min_l2_confidence = 0.0;

    fob::venue::v1::VenueLiquidityCurve l2;
    pass = Check(publish_curve(l2_cfg, &l2), "L2 publish + parse must succeed") && pass;
    pass = Check(l2.level() == "L2", "L2 level must stay L2") && pass;
    pass = Check(IsFiniteNonNegative(l2.epsilon1()), "L2 epsilon1 must be finite >= 0") && pass;
    pass = Check(IsFiniteNonNegative(l2.epsilon2()), "L2 epsilon2 must be finite >= 0") && pass;
    pass = Check(IsFiniteNonNegative(l2.epsilon3()), "L2 epsilon3 must be finite >= 0") && pass;
    pass = Check(l2.confidence() >= 0.0 && l2.confidence() <= 1.0,
                 "L2 confidence must be in [0,1]") && pass;
  }

  {
    LiquidityCurveProducerConfig l3_cfg;
    l3_cfg.level = "L3";
    l3_cfg.l3_impact.enabled = true;
    l3_cfg.degradation.min_l3_confidence = 0.0;
    l3_cfg.degradation.min_l2_confidence = 0.0;
    l3_cfg.degradation.min_l1_confidence = 0.0;
    l3_cfg.degradation.epsilon1_green_bps = 1.0e6;
    l3_cfg.degradation.epsilon2_green_bps = 1.0e6;
    l3_cfg.degradation.epsilon3_green_bps = 1.0e6;

    fob::venue::v1::VenueLiquidityCurve l3;
    pass = Check(publish_curve(l3_cfg, &l3), "L3 publish + parse must succeed") && pass;
    pass = Check(l3.level() == "L3", "L3 level must stay L3") && pass;
    pass = Check(IsFiniteNonNegative(l3.epsilon1()), "L3 epsilon1 must be finite >= 0") && pass;
    pass = Check(IsFiniteNonNegative(l3.epsilon2()), "L3 epsilon2 must be finite >= 0") && pass;
    pass = Check(IsFiniteNonNegative(l3.epsilon3()), "L3 epsilon3 must be finite >= 0") && pass;
    pass = Check(l3.confidence() >= 0.0 && l3.confidence() <= 1.0,
                 "L3 confidence must be in [0,1]") && pass;
  }

  return pass;
}

bool TestRepresentativeL1L2LatencyUnderFiftyMs() {
  const auto run_case = [](const std::string& level) {
    FakePublisher publisher;
    LiquidityCurveProducerConfig cfg;
    cfg.level = level;
    cfg.degradation.min_l1_confidence = 0.0;
    cfg.degradation.min_l2_confidence = 0.0;
    LiquidityCurveProducer producer(&publisher, cfg);

    std::vector<double> samples_ms;
    samples_ms.reserve(20);
    for (int i = 0; i < 20; ++i) {
      const auto started = std::chrono::steady_clock::now();
      const bool ok = producer.Publish(MakeSnapshot());
      const auto finished = std::chrono::steady_clock::now();
      if (!ok) return 1.0e9;
      samples_ms.push_back(
          static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                  finished - started)
                                  .count()) /
          1000.0);
    }
    std::sort(samples_ms.begin(), samples_ms.end());
    const std::size_t idx = static_cast<std::size_t>(
        std::ceil(0.95 * static_cast<double>(samples_ms.size()))) - 1;
    return samples_ms[std::min(idx, samples_ms.size() - 1)];
  };

  const double l1_p95_ms = run_case("L1");
  const double l2_p95_ms = run_case("L2");

  bool pass = true;
  pass = Check(l1_p95_ms < 50.0, "representative L1 p95 latency must be <50ms") && pass;
  pass = Check(l2_p95_ms < 50.0, "representative L2 p95 latency must be <50ms") && pass;
  return pass;
}

bool TestL3ImpactCalibrationModels() {
  const std::vector<L3ImpactModel> models{
      L3ImpactModel::kLinear,
      L3ImpactModel::kQuadratic,
      L3ImpactModel::kSqrt,
  };

  bool pass = true;
  for (const L3ImpactModel model : models) {
    FakePublisher publisher;
    LiquidityCurveProducerConfig cfg;
    cfg.topic = "venue.liquidity.fob";
    cfg.level = "L3";
    cfg.tau_ms = 1000.0;
    cfg.apply_convexification = false;
    cfg.apply_moreau_l2 = false;
    cfg.apply_tikhonov_l2 = false;
    cfg.apply_fenchel_legendre = true;
    cfg.l3_impact.enabled = true;
    cfg.l3_impact.model = model;
    cfg.l3_impact.max_history = 32;
    cfg.l3_impact.max_relative_impact = 0.5;
    cfg.l3_impact.execution_blend_weight = 1.0;
    cfg.l3_impact.min_samples_for_full_weight = 1;
    cfg.degradation.epsilon1_green_bps = 1000.0;
    cfg.degradation.epsilon2_green_bps = 1000.0;
    cfg.degradation.epsilon3_green_bps = 1000.0;
    cfg.degradation.min_l3_confidence = 0.0;

    LiquidityCurveProducer producer(&publisher, cfg);
    const bool base_ok = producer.Publish(MakeSnapshot());
    pass = Check(base_ok, "L3 baseline publish must succeed") && pass;
    if (!base_ok || publisher.messages.empty()) {
      continue;
    }

    fob::venue::v1::VenueLiquidityCurve base_curve;
    pass = Check(cex::common::from_bytes(publisher.messages.back().payload, base_curve),
                 "L3 baseline payload parse") && pass;
    if (base_curve.ask_curve().p_of_q_size() == 0) {
      pass = Check(false, "L3 baseline ask curve must not be empty") && pass;
      continue;
    }
    const double base_last_ask = base_curve.ask_curve().p_of_q(
        base_curve.ask_curve().p_of_q_size() - 1);

    const auto intent = MakeIntent(fob::common::v1::SIDE_BUY, 2);
    producer.ObserveExecution(intent, MakeReport(2, 103));
    producer.ObserveExecution(intent, MakeReport(2, 104));
    producer.ObserveExecution(intent, MakeReport(2, 105));

    const bool calibrated_ok = producer.Publish(MakeSnapshot());
    pass = Check(calibrated_ok, "L3 calibrated publish must succeed") && pass;
    if (!calibrated_ok || publisher.messages.size() < 2) {
      continue;
    }

    fob::venue::v1::VenueLiquidityCurve calibrated_curve;
    pass = Check(cex::common::from_bytes(publisher.messages.back().payload, calibrated_curve),
                 "L3 calibrated payload parse") && pass;
    if (calibrated_curve.ask_curve().p_of_q_size() == 0) {
      pass = Check(false, "L3 calibrated ask curve must not be empty") && pass;
      continue;
    }

    const double calibrated_last_ask = calibrated_curve.ask_curve().p_of_q(
        calibrated_curve.ask_curve().p_of_q_size() - 1);
    pass = Check(calibrated_curve.level() == "L3", "curve level must be L3") && pass;
    pass = Check(calibrated_last_ask > base_last_ask,
                 "calibrated ask impact must increase marginal ask price") && pass;
    pass = Check(calibrated_curve.confidence() >= 0.0 && calibrated_curve.confidence() <= 1.0,
                 "L3 confidence must be in [0,1]") && pass;
  }

  return pass;
}

bool TestL3CalibratedUpdatesEpsilon3AndConfidence() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.level = "L3";
  cfg.tau_ms = 1000.0;
  cfg.apply_convexification = true;
  cfg.apply_moreau_l2 = true;
  cfg.apply_tikhonov_l2 = true;
  cfg.apply_fenchel_legendre = true;
  cfg.l3_impact.enabled = true;
  cfg.l3_impact.model = L3ImpactModel::kLinear;
  cfg.l3_impact.max_history = 64;
  cfg.l3_impact.max_relative_impact = 0.5;
  cfg.l3_impact.execution_blend_weight = 0.8;
  cfg.l3_impact.min_samples_for_full_weight = 2;
  cfg.degradation.epsilon1_green_bps = 1000.0;
  cfg.degradation.epsilon2_green_bps = 1000.0;
  cfg.degradation.epsilon3_green_bps = 1000.0;
  cfg.degradation.min_l3_confidence = 0.0;

  LiquidityCurveProducer producer(&publisher, cfg);

  const bool base_ok = producer.Publish(MakeSnapshot());
  if (!Check(base_ok, "L3 calibrated baseline publish must succeed")) return false;
  if (!Check(!publisher.messages.empty(), "baseline message must exist")) return false;

  fob::venue::v1::VenueLiquidityCurve before;
  if (!Check(cex::common::from_bytes(publisher.messages.back().payload, before),
             "baseline L3 curve parse")) {
    return false;
  }
  const double eps3_before = before.epsilon3();
  const double conf_before = before.confidence();

  const auto intent = MakeIntent(fob::common::v1::SIDE_BUY, 2);
  producer.ObserveExecution(intent, MakeReport(2, 10101, 0, 2));
  producer.ObserveExecution(intent, MakeReport(2, 10101, 0, 2));
  producer.ObserveExecution(intent, MakeReport(2, 10101, 0, 2));
  producer.ObserveExecution(intent, MakeReport(2, 10101, 0, 2));

  const bool after_ok = producer.Publish(MakeSnapshot());
  if (!Check(after_ok, "L3 calibrated publish after history must succeed")) return false;
  if (!Check(publisher.messages.size() >= 2, "must have second message")) return false;

  fob::venue::v1::VenueLiquidityCurve after;
  if (!Check(cex::common::from_bytes(publisher.messages.back().payload, after),
             "calibrated L3 curve parse")) {
    return false;
  }

  bool pass = true;
  pass = Check(after.level() == "L3", "L3 calibrated level must stay L3") && pass;
  pass = Check(after.epsilon3() >= eps3_before,
               "L3 epsilon3 must become execution-derived after history") && pass;
  pass = Check(after.confidence() >= 0.0 && after.confidence() <= 1.0 &&
                   after.confidence() != conf_before,
               "L3 calibrated confidence must update after execution history") && pass;
  return pass;
}

bool TestQualityGatingDegradesL3ToL2() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.level = "L3";
  cfg.tau_ms = 1000.0;
  cfg.apply_convexification = true;
  cfg.apply_moreau_l2 = true;
  cfg.apply_tikhonov_l2 = true;
  cfg.apply_fenchel_legendre = true;
  cfg.l3_impact.enabled = true;
  cfg.l3_impact.model = L3ImpactModel::kLinear;
  cfg.enable_quality_gating = true;
  cfg.quality_thresholds.epsilon1_degrade = 1.0;
  cfg.quality_thresholds.epsilon1_disable = 2.0;
  cfg.quality_thresholds.epsilon2_degrade = 1.0;
  cfg.quality_thresholds.epsilon2_disable = 2.0;
  cfg.quality_thresholds.epsilon3_degrade = 0.2;
  cfg.quality_thresholds.epsilon3_disable = 0.95;
  cfg.degradation.min_l2_confidence = 0.0;
  cfg.degradation.min_l1_confidence = 0.0;

  LiquidityCurveProducer producer(&publisher, cfg);
  const bool ok = producer.Publish(MakeSnapshot());
  if (!Check(ok, "Quality gating degrade publish must succeed")) return false;
  if (!Check(publisher.messages.size() == 1, "Exactly one degraded message expected")) return false;

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages.back().payload, curve),
             "Degraded curve payload parse")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.level() == "L2", "L3 should degrade to L2 by epsilon3 threshold") && pass;
  pass = Check(curve.confidence() >= 0.0 && curve.confidence() <= 1.0,
               "Legacy quality-gated confidence must be in [0,1]") && pass;
  return pass;
}

bool TestQualityGatingDisableSkipsPublish() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.level = "L2";
  cfg.tau_ms = 1000.0;
  cfg.apply_convexification = true;
  cfg.apply_moreau_l2 = true;
  cfg.apply_tikhonov_l2 = true;
  cfg.apply_fenchel_legendre = false;
  cfg.enable_quality_gating = true;
  cfg.quality_thresholds.epsilon1_degrade = 1.0;
  cfg.quality_thresholds.epsilon1_disable = 2.0;
  cfg.quality_thresholds.epsilon2_degrade = 1.0;
  cfg.quality_thresholds.epsilon2_disable = 2.0;
  cfg.quality_thresholds.epsilon3_degrade = 0.2;
  cfg.quality_thresholds.epsilon3_disable = 0.9;
  cfg.degradation.min_l2_confidence = 0.0;
  cfg.degradation.min_l1_confidence = 0.0;

  LiquidityCurveProducer producer(&publisher, cfg);
  const bool ok = producer.Publish(MakeSnapshot());

  bool pass = true;
  pass = Check(!ok, "Quality gating disable must skip publish") && pass;
  pass = Check(publisher.messages.empty(), "No messages must be produced on disable") && pass;
  return pass;
}

bool TestQualityDegradesL3ToL2() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.level = "L3";
  cfg.l3_impact.enabled = true;
  cfg.apply_fenchel_legendre = true;
  cfg.degradation.min_l3_confidence = 0.99;
  cfg.degradation.min_l2_confidence = 0.0;
  cfg.degradation.min_l1_confidence = 0.0;

  LiquidityCurveProducer producer(&publisher, cfg);
  const bool ok = producer.Publish(MakeSnapshot());
  if (!Check(ok, "quality-degraded publish must succeed")) return false;
  if (!Check(publisher.messages.size() == 1, "quality-degraded message expected")) return false;

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages.back().payload, curve),
             "quality-degraded curve parse")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.level() == "L2", "poor L3 confidence must degrade to L2") && pass;
  pass = Check(curve.confidence() >= 0.0 && curve.confidence() <= 1.0,
               "degraded L2 confidence must be in [0,1]") && pass;
  return pass;
}

bool TestStaleDegradesToL1WithLowerConfidence() {
  FakePublisher fresh_publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.level = "L3";
  cfg.l3_impact.enabled = true;
  cfg.degradation.min_l3_confidence = 0.0;
  cfg.degradation.min_l2_confidence = 0.0;
  cfg.degradation.min_l1_confidence = 0.0;
  cfg.degradation.epsilon1_green_bps = 1.0e6;
  cfg.degradation.epsilon2_green_bps = 1.0e6;
  cfg.degradation.epsilon3_green_bps = 1.0e6;
  cfg.degradation.publish_stale_l1_fallback = true;

  LiquidityCurveProducer fresh_producer(&fresh_publisher, cfg);
  const bool fresh_ok = fresh_producer.Publish(MakeSnapshot());
  if (!Check(fresh_ok, "fresh publish must succeed")) return false;
  fob::venue::v1::VenueLiquidityCurve fresh_curve;
  if (!Check(cex::common::from_bytes(fresh_publisher.messages.back().payload, fresh_curve),
             "fresh curve parse")) {
    return false;
  }

  FakePublisher stale_publisher;
  LiquidityCurveProducer stale_producer(&stale_publisher, cfg);
  auto stale = MakeSnapshot();
  stale.set_status("stale");
  const bool stale_ok = stale_producer.Publish(stale);
  if (!Check(stale_ok, "stale publish should produce degraded fallback")) return false;
  fob::venue::v1::VenueLiquidityCurve stale_curve;
  if (!Check(cex::common::from_bytes(stale_publisher.messages.back().payload, stale_curve),
             "stale curve parse")) {
    return false;
  }

  bool pass = true;
  pass = Check(stale_curve.level() == "L1", "stale snapshot must cap curve at L1") && pass;
  pass = Check(stale_curve.confidence() < fresh_curve.confidence(),
               "stale confidence must be lower than fresh confidence") && pass;
  return pass;
}

bool TestExecutionErrorsDegradeToL1ThenOff() {
  FakePublisher publisher;
  LiquidityCurveProducerConfig cfg;
  cfg.level = "L3";
  cfg.l3_impact.enabled = true;
  cfg.degradation.min_l3_confidence = 0.0;
  cfg.degradation.min_l2_confidence = 0.0;
  cfg.degradation.min_l1_confidence = 0.0;
  cfg.degradation.max_consecutive_errors_off = 3;

  LiquidityCurveProducer producer(&publisher, cfg);
  const auto intent = MakeIntent(fob::common::v1::SIDE_BUY, 2);
  producer.ObserveExecution(intent, MakeRejectedReport());
  producer.ObserveExecution(intent, MakeRejectedReport());

  const bool degraded_ok = producer.Publish(MakeSnapshot());
  if (!Check(degraded_ok, "two execution errors should degrade but still publish")) return false;

  fob::venue::v1::VenueLiquidityCurve degraded_curve;
  if (!Check(cex::common::from_bytes(publisher.messages.back().payload, degraded_curve),
             "error-degraded curve parse")) {
    return false;
  }

  bool pass = true;
  pass = Check(degraded_curve.level() == "L1",
               "two consecutive errors must degrade L3 to L1") && pass;

  producer.ObserveExecution(intent, MakeRejectedReport());
  const std::size_t before_off_messages = publisher.messages.size();
  const bool off_ok = producer.Publish(MakeSnapshot());
  pass = Check(!off_ok, "three consecutive errors must degrade to OFF") && pass;
  pass = Check(publisher.messages.size() == before_off_messages,
               "OFF must not publish a new curve") && pass;
  return pass;
}

bool TestPublishesSyntheticFlowOrdersAndStoresRows() {
  FakePublisher publisher;
  FakeSyntheticOrderRepository repository;
  LiquidityCurveProducerConfig cfg;
  cfg.level = "L1";
  cfg.tau_ms = 1000.0;
  cfg.synthetic.enabled = true;
  cfg.synthetic.topic = "venue.synthetic";
  cfg.synthetic.liquidity_source = "cex_hedge";
  cfg.synthetic.ttl_ms = 1500;
  cfg.degradation.min_l1_confidence = 0.0;

  LiquidityCurveProducer producer(&publisher, &repository, cfg);
  const bool ok = producer.Publish(MakeSnapshot());
  if (!Check(ok, "synthetic publish must succeed")) return false;
  if (!Check(publisher.messages.size() == 3,
             "curve plus two synthetic messages expected")) {
    return false;
  }
  if (!Check(repository.orders.size() == 2,
             "two synthetic_orders rows must be saved")) {
    return false;
  }

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages[0].payload, curve),
             "curve payload parse for synthetic test")) {
    return false;
  }

  std::vector<fob::orders::v1::SyntheticFlowOrder> synthetics;
  for (std::size_t i = 1; i < publisher.messages.size(); ++i) {
    if (!Check(publisher.messages[i].topic == "venue.synthetic",
               "synthetic topic mismatch")) {
      return false;
    }
    fob::orders::v1::SyntheticFlowOrder order;
    if (!Check(cex::common::from_bytes(publisher.messages[i].payload, order),
               "SyntheticFlowOrder payload parse")) {
      return false;
    }
    synthetics.push_back(order);
  }

  const fob::orders::v1::SyntheticFlowOrder* buy = nullptr;
  const fob::orders::v1::SyntheticFlowOrder* sell = nullptr;
  for (const auto& order : synthetics) {
    if (order.side() == fob::common::v1::SIDE_BUY) buy = &order;
    if (order.side() == fob::common::v1::SIDE_SELL) sell = &order;
  }

  bool pass = true;
  pass = Check(buy != nullptr, "bid curve must produce BUY synthetic order") && pass;
  pass = Check(sell != nullptr, "ask curve must produce SELL synthetic order") && pass;
  if (buy == nullptr || sell == nullptr) return false;

  for (const auto* order : {buy, sell}) {
    const double p_l = DecAsDouble(order->p_l());
    const double p_h = DecAsDouble(order->p_h());
    const double q_rate = DecAsDouble(order->q_rate());
    const double q_max = DecAsDouble(order->q_max());

    pass = Check(!order->synthetic_id().empty(), "synthetic_id must be set") && pass;
    pass = Check(order->venue_id() == "binance", "venue_id must be copied to synthetic") && pass;
    pass = Check(order->instrument().symbol() == "BTC/USDT",
                 "symbol must be copied to synthetic") && pass;
    pass = Check(order->curve_id() == curve.curve_id(),
                 "curve_id must link to source curve") && pass;
    pass = Check(order->snapshot_id() == "snapshot-1",
                 "snapshot_id must link to source snapshot") && pass;
    pass = Check(order->liquidity_source() == "cex_hedge",
                 "liquidity_source must be configured") && pass;
    pass = Check(order->status() == "active", "synthetic status must be active") && pass;
    pass = Check(order->expires_at().seconds() > order->created_at().seconds() ||
                     order->expires_at().nanos() > order->created_at().nanos(),
                 "expires_at must be after created_at") && pass;
    pass = Check(p_l > 0.0 && p_h >= p_l, "synthetic price interval must be valid") && pass;
    pass = Check(q_max > 0.0 && std::fabs(q_rate - q_max) < 1e-9,
                 "q_rate must equal q_max for tau=1s") && pass;

    pass = Check(order->order().order_id() == order->synthetic_id(),
                 "embedded FlowOrder order_id must equal synthetic_id") && pass;
    pass = Check(order->order().side() == order->side(),
                 "embedded FlowOrder side must match wrapper") && pass;
    pass = Check(DecAsDouble(order->order().price_low()) == p_l,
                 "embedded FlowOrder p_low must match wrapper") && pass;
    pass = Check(DecAsDouble(order->order().price_high()) == p_h,
                 "embedded FlowOrder p_high must match wrapper") && pass;
    pass = Check(DecAsDouble(order->order().total_qty()) == q_max,
                 "embedded FlowOrder q_max must match wrapper") && pass;
    pass = Check(DecAsDouble(order->order().max_speed()) == q_rate,
                 "embedded FlowOrder q_rate must match wrapper") && pass;
    pass = Check(order->order().status() == fob::common::v1::ORDER_STATUS_UNSPECIFIED,
                 "embedded FlowOrder must map to active matching status") && pass;
  }

  pass = Check(repository.orders[0].synthetic_id() == synthetics[0].synthetic_id() &&
                   repository.orders[1].synthetic_id() == synthetics[1].synthetic_id(),
               "repository must receive the published synthetic orders") && pass;
  return pass;
}

bool TestPublishOptionsOverrideLevelAndSyntheticOutput() {
  FakePublisher publisher;
  FakeSyntheticOrderRepository repository;
  LiquidityCurveProducerConfig cfg;
  cfg.topic = "venue.liquidity.fob";
  cfg.level = "L3";
  cfg.l3_impact.enabled = true;
  cfg.synthetic.enabled = false;
  cfg.synthetic.topic = "venue.synthetic";
  cfg.synthetic.liquidity_source = "cex_hedge";
  cfg.degradation.min_l1_confidence = 0.0;

  LiquidityCurveProducer producer(&publisher, &repository, cfg);

  LiquidityCurveProducer::PublishOptions options;
  options.requested_level = "L1";
  options.synthetic_enabled = true;

  fob::venue::v1::VenueLiquidityCurve curve;
  std::vector<fob::orders::v1::SyntheticFlowOrder> synthetics;
  const bool ok = producer.Publish(MakeSnapshot(), &curve, &synthetics, options);
  if (!Check(ok, "publish override must succeed")) return false;
  if (!Check(publisher.messages.size() == 3,
             "curve plus two synthetic messages expected with synthetic override")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.level() == "L1", "requested level override must force L1 curve") && pass;
  pass = Check(Tag(curve.meta(), "requested_level") == "L1",
               "requested level tag must reflect override") && pass;
  pass = Check(Tag(curve.meta(), "effective_level") == "L1",
               "effective level tag must reflect override") && pass;
  pass = Check(synthetics.size() == 2, "synthetic override must emit two orders") && pass;
  pass = Check(repository.orders.size() == 2,
               "synthetic override must persist two synthetic rows") && pass;
  return pass;
}

bool TestVolume24hCapsSyntheticSpeed() {
  FakePublisher publisher;
  FakeSyntheticOrderRepository repository;
  LiquidityCurveProducerConfig cfg;
  cfg.level = "L1";
  cfg.tau_ms = 5000.0;
  cfg.synthetic.enabled = true;
  cfg.synthetic.topic = "venue.synthetic";
  cfg.synthetic.liquidity_source = "cex_hedge";
  cfg.degradation.min_l1_confidence = 0.0;

  fob::venue::v1::VenueSnapshot snapshot = MakeSnapshot();
  *snapshot.mutable_volume_24h() = Dec(1, 0);

  LiquidityCurveProducer producer(&publisher, &repository, cfg);
  const bool ok = producer.Publish(snapshot);
  if (!Check(ok, "volume_24h capped publish must succeed")) return false;
  if (!Check(publisher.messages.size() == 3,
             "curve plus two capped synthetic messages expected")) {
    return false;
  }

  fob::venue::v1::VenueLiquidityCurve curve;
  if (!Check(cex::common::from_bytes(publisher.messages[0].payload, curve),
             "curve payload parse for turnover cap test")) {
    return false;
  }

  bool pass = true;
  pass = Check(curve.tau_ms() > cfg.tau_ms,
               "effective tau must be increased when q_rate exceeds 24h turnover cap") && pass;
  pass = Check(Tag(curve.meta(), "tau_adjustment_reason") == "volume_24h_turnover_cap",
               "tau_adjustment_reason tag must explain turnover cap") && pass;
  pass = Check(std::fabs(TagAsDouble(curve.meta(), "hourly_turnover_cap", 0.0) - 1.0) < 1e-9,
               "hourly_turnover_cap must match base 24h volume") && pass;

  for (std::size_t i = 1; i < publisher.messages.size(); ++i) {
    fob::orders::v1::SyntheticFlowOrder order;
    if (!Check(cex::common::from_bytes(publisher.messages[i].payload, order),
               "SyntheticFlowOrder payload parse for turnover cap test")) {
      return false;
    }
    const double q_rate = DecAsDouble(order.q_rate());
    pass = Check(q_rate > 0.0 && q_rate <= 1.0 + 1e-9,
                 "synthetic q_rate must be capped by 24h turnover-derived hourly limit") && pass;
  }
  return pass;
}

bool TestStaleSnapshotDoesNotPublishCurveOrSyntheticByDefault() {
  FakePublisher publisher;
  FakeSyntheticOrderRepository repository;
  LiquidityCurveProducerConfig cfg;
  cfg.level = "L3";
  cfg.l3_impact.enabled = true;
  cfg.synthetic.enabled = true;
  cfg.degradation.min_l1_confidence = 0.0;
  cfg.degradation.min_l2_confidence = 0.0;
  cfg.degradation.min_l3_confidence = 0.0;

  LiquidityCurveProducer producer(&publisher, &repository, cfg);
  auto stale = MakeSnapshot();
  stale.set_status("stale");
  const bool ok = producer.Publish(stale);

  bool pass = true;
  pass = Check(!ok, "stale snapshot must not publish active FOB liquidity by default") && pass;
  pass = Check(publisher.messages.empty(),
               "stale snapshot must not publish curve or venue.synthetic") && pass;
  pass = Check(repository.orders.empty(),
               "stale snapshot must not write synthetic_orders") && pass;
  return pass;
}

bool TestEmptyStatusDoesNotPublishEvenWithBook() {
  FakePublisher publisher;
  FakeSyntheticOrderRepository repository;
  LiquidityCurveProducerConfig cfg;
  cfg.level = "L2";
  cfg.synthetic.enabled = true;
  cfg.degradation.enabled = false;

  LiquidityCurveProducer producer(&publisher, &repository, cfg);
  auto empty = MakeSnapshot();
  empty.set_status("empty");
  const bool ok = producer.Publish(empty);

  bool pass = true;
  pass = Check(!ok, "empty status must not publish VenueLiquidityCurve") && pass;
  pass = Check(publisher.messages.empty(),
               "empty status must not publish curve or synthetic orders") && pass;
  pass = Check(repository.orders.empty(),
               "empty status must not write synthetic_orders") && pass;
  return pass;
}

bool TestRejectsEmptyBook() {
  FakePublisher publisher;
  LiquidityCurveProducer producer(&publisher, {});

  fob::venue::v1::VenueSnapshot empty;
  empty.set_venue_id("binance");
  empty.mutable_instrument()->set_symbol("BTC/USDT");
  empty.mutable_instrument()->set_base("BTC");
  empty.mutable_instrument()->set_quote("USDT");

  const bool ok = producer.Publish(empty);
  bool pass = true;
  pass = Check(!ok, "Publish must fail on empty book") && pass;
  pass = Check(publisher.messages.empty(), "No Kafka messages must be produced") && pass;
  return pass;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestPublishesRegularizedCurve() && ok;
  ok = TestPublishesL2ForHighPriceSnapshot() && ok;
  ok = TestLiveLikeHighScaleSnapshotRetainsL2() && ok;
  ok = TestLiveLikeHighScaleSnapshotCanBuildL3() && ok;
  ok = TestLiveLikeL3CalibrationDoesNotSaturateCostLayer() && ok;
  ok = TestCapturedBinanceSnapshotRetainsL2() && ok;
  ok = TestAmmDirectPipelineBuildsCurveAndPublishesComparison() && ok;
  ok = TestAmmDirectCorrectionsAdjustCurveAndPublishCalibrationMeta() && ok;
  ok = TestLiquidityFobVersioningMetadataIsConfigurable() && ok;
  ok = TestTakerFeeAdjustsEffectiveCurves() && ok;
  ok = TestMinQtyFiltersDustLevels() && ok;
  ok = TestL1L2L3LevelsAndQualityMetrics() && ok;
  ok = TestRepresentativeL1L2LatencyUnderFiftyMs() && ok;
  ok = TestL3ImpactCalibrationModels() && ok;
  ok = TestL3CalibratedUpdatesEpsilon3AndConfidence() && ok;
  ok = TestQualityGatingDegradesL3ToL2() && ok;
  ok = TestQualityGatingDisableSkipsPublish() && ok;
  ok = TestQualityDegradesL3ToL2() && ok;
  ok = TestStaleDegradesToL1WithLowerConfidence() && ok;
  ok = TestExecutionErrorsDegradeToL1ThenOff() && ok;
  ok = TestPublishOptionsOverrideLevelAndSyntheticOutput() && ok;
  ok = TestPublishesSyntheticFlowOrdersAndStoresRows() && ok;
  ok = TestVolume24hCapsSyntheticSpeed() && ok;
  ok = TestStaleSnapshotDoesNotPublishCurveOrSyntheticByDefault() && ok;
  ok = TestEmptyStatusDoesNotPublishEvenWithBook() && ok;
  ok = TestRejectsEmptyBook() && ok;
  if (ok) {
    std::cout << "[OK] liquidity_curve_producer_test passed" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
