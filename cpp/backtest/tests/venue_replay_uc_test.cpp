#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "app/venue_replay_uc.hpp"

namespace {

struct FakeVenueStorage final : public cex::backtest::app::IVenueReplayStorage {
  int save_snapshot_calls{0};
  int save_curve_calls{0};
  std::string last_venue_id;
  std::string last_symbol;
  bool fail_snapshot{false};
  bool fail_curve{false};

  bool SaveVenueSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) override {
    ++save_snapshot_calls;
    last_venue_id = snapshot.venue_id();
    last_symbol = snapshot.instrument().symbol();
    return !fail_snapshot;
  }

  bool SaveVenueLiquidityCurve(const fob::venue::v1::VenueLiquidityCurve& curve) override {
    ++save_curve_calls;
    last_venue_id = curve.venue_id();
    last_symbol = curve.instrument().symbol();
    return !fail_curve;
  }
};

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

fob::venue::v1::VenueSnapshot MakeSnapshot(
    const std::string& venue_id, const std::string& symbol,
    const std::string& status = "connected") {
  fob::venue::v1::VenueSnapshot snapshot;
  snapshot.set_venue_id(venue_id);
  snapshot.mutable_instrument()->set_symbol(symbol);
  snapshot.mutable_instrument()->set_base("BTC");
  snapshot.mutable_instrument()->set_quote("USDT");
  snapshot.set_status(status);

  snapshot.mutable_best_bid()->set_units(7000000);
  snapshot.mutable_best_bid()->set_scale(2);
  snapshot.mutable_best_ask()->set_units(7001000);
  snapshot.mutable_best_ask()->set_scale(2);
  snapshot.mutable_mid_price()->set_units(7000500);
  snapshot.mutable_mid_price()->set_scale(2);
  snapshot.mutable_spread()->set_units(1000);
  snapshot.mutable_spread()->set_scale(2);

  snapshot.mutable_maker_fee()->set_units(10);
  snapshot.mutable_maker_fee()->set_scale(4);
  snapshot.mutable_taker_fee()->set_units(15);
  snapshot.mutable_taker_fee()->set_scale(4);
  snapshot.mutable_tick_size()->set_units(1);
  snapshot.mutable_tick_size()->set_scale(2);
  snapshot.mutable_lot_size()->set_units(1);
  snapshot.mutable_lot_size()->set_scale(6);

  snapshot.mutable_timestamp()->set_seconds(1700000000);
  snapshot.mutable_timestamp()->set_nanos(0);

  return snapshot;
}

fob::venue::v1::VenueLiquidityCurve MakeCurve(
    const std::string& venue_id, const std::string& symbol,
    const std::string& level = "L1") {
  fob::venue::v1::VenueLiquidityCurve curve;
  curve.set_venue_id(venue_id);
  curve.mutable_instrument()->set_symbol(symbol);
  curve.mutable_instrument()->set_base("BTC");
  curve.mutable_instrument()->set_quote("USDT");
  curve.set_snapshot_id("snap-123");
  curve.set_level(level);
  curve.set_epsilon1(0.001);
  curve.set_epsilon2(0.002);
  curve.set_epsilon3(0.0);
  curve.set_confidence(0.95);
  curve.set_tau_ms(100.0);
  curve.mutable_mid_price()->set_units(7000500);
  curve.mutable_mid_price()->set_scale(2);

  auto* bid = curve.mutable_bid_curve();
  bid->add_q_grid(0.0);
  bid->add_q_grid(1.0);
  bid->add_q_grid(5.0);
  bid->add_p_of_q(70005.0);
  bid->add_p_of_q(70000.0);
  bid->add_p_of_q(69950.0);
  bid->add_s_of_q(0.0);
  bid->add_s_of_q(70000.0);
  bid->add_s_of_q(349750.0);

  auto* ask = curve.mutable_ask_curve();
  ask->add_q_grid(0.0);
  ask->add_q_grid(1.0);
  ask->add_q_grid(5.0);
  ask->add_p_of_q(70010.0);
  ask->add_p_of_q(70050.0);
  ask->add_p_of_q(70200.0);
  ask->add_s_of_q(0.0);
  ask->add_s_of_q(70050.0);
  ask->add_s_of_q(350500.0);

  curve.mutable_timestamp()->set_seconds(1700000000);
  curve.mutable_timestamp()->set_nanos(0);

  return curve;
}

// --- Tests ---

bool test_snapshot_persisted() {
  FakeVenueStorage storage;
  cex::backtest::app::VenueReplayUseCases uc(&storage);

  auto snapshot = MakeSnapshot("binance", "BTC/USDT");
  uc.OnVenueSnapshot(snapshot);

  if (!Check(storage.save_snapshot_calls == 1, "SaveVenueSnapshot must be called once"))
    return false;
  if (!Check(storage.last_venue_id == "binance", "venue_id must be binance"))
    return false;
  if (!Check(storage.last_symbol == "BTC/USDT", "symbol must be BTC/USDT"))
    return false;

  auto stats = uc.GetStats();
  if (!Check(stats.snapshots_received == 1, "snapshots_received must be 1"))
    return false;
  if (!Check(stats.snapshots_saved == 1, "snapshots_saved must be 1"))
    return false;
  if (!Check(stats.last_venue_id == "binance", "last_venue_id must be binance"))
    return false;
  if (!Check(stats.last_symbol == "BTC/USDT", "last_symbol must be BTC/USDT"))
    return false;
  return true;
}

bool test_curve_persisted() {
  FakeVenueStorage storage;
  cex::backtest::app::VenueReplayUseCases uc(&storage);

  auto curve = MakeCurve("binance", "BTC/USDT", "L1");
  uc.OnVenueLiquidityCurve(curve);

  if (!Check(storage.save_curve_calls == 1, "SaveVenueLiquidityCurve must be called once"))
    return false;
  if (!Check(storage.last_venue_id == "binance", "venue_id must be binance"))
    return false;
  if (!Check(storage.last_symbol == "BTC/USDT", "symbol must be BTC/USDT"))
    return false;

  auto stats = uc.GetStats();
  if (!Check(stats.curves_received == 1, "curves_received must be 1"))
    return false;
  if (!Check(stats.curves_saved == 1, "curves_saved must be 1"))
    return false;
  return true;
}

bool test_multiple_snapshots_and_curves() {
  FakeVenueStorage storage;
  cex::backtest::app::VenueReplayUseCases uc(&storage);

  uc.OnVenueSnapshot(MakeSnapshot("binance", "BTC/USDT"));
  uc.OnVenueSnapshot(MakeSnapshot("coinbase", "ETH/USDT"));
  uc.OnVenueLiquidityCurve(MakeCurve("binance", "BTC/USDT", "L1"));
  uc.OnVenueLiquidityCurve(MakeCurve("coinbase", "ETH/USDT", "L2"));
  uc.OnVenueLiquidityCurve(MakeCurve("binance", "BTC/USDT", "L1"));

  if (!Check(storage.save_snapshot_calls == 2, "SaveVenueSnapshot must be called 2 times"))
    return false;
  if (!Check(storage.save_curve_calls == 3, "SaveVenueLiquidityCurve must be called 3 times"))
    return false;

  auto stats = uc.GetStats();
  if (!Check(stats.snapshots_received == 2, "snapshots_received must be 2"))
    return false;
  if (!Check(stats.snapshots_saved == 2, "snapshots_saved must be 2"))
    return false;
  if (!Check(stats.curves_received == 3, "curves_received must be 3"))
    return false;
  if (!Check(stats.curves_saved == 3, "curves_saved must be 3"))
    return false;
  if (!Check(stats.last_venue_id == "binance", "last_venue_id must be binance"))
    return false;
  if (!Check(stats.last_symbol == "BTC/USDT", "last_symbol must be BTC/USDT"))
    return false;
  return true;
}

bool test_no_storage_configured() {
  cex::backtest::app::VenueReplayUseCases uc(nullptr);

  uc.OnVenueSnapshot(MakeSnapshot("binance", "BTC/USDT"));
  uc.OnVenueLiquidityCurve(MakeCurve("binance", "BTC/USDT"));

  auto stats = uc.GetStats();
  if (!Check(stats.snapshots_received == 1, "snapshots_received must be 1 even without storage"))
    return false;
  if (!Check(stats.snapshots_saved == 0, "snapshots_saved must be 0 without storage"))
    return false;
  if (!Check(stats.curves_received == 1, "curves_received must be 1 even without storage"))
    return false;
  if (!Check(stats.curves_saved == 0, "curves_saved must be 0 without storage"))
    return false;
  return true;
}

bool test_snapshot_storage_failure() {
  FakeVenueStorage storage;
  storage.fail_snapshot = true;
  cex::backtest::app::VenueReplayUseCases uc(&storage);

  uc.OnVenueSnapshot(MakeSnapshot("binance", "BTC/USDT"));

  if (!Check(storage.save_snapshot_calls == 1, "SaveVenueSnapshot must be called"))
    return false;

  auto stats = uc.GetStats();
  if (!Check(stats.snapshots_received == 1, "snapshots_received must be 1"))
    return false;
  if (!Check(stats.snapshots_saved == 0, "snapshots_saved must be 0 on failure"))
    return false;
  return true;
}

bool test_curve_storage_failure() {
  FakeVenueStorage storage;
  storage.fail_curve = true;
  cex::backtest::app::VenueReplayUseCases uc(&storage);

  uc.OnVenueLiquidityCurve(MakeCurve("binance", "BTC/USDT"));

  if (!Check(storage.save_curve_calls == 1, "SaveVenueLiquidityCurve must be called"))
    return false;

  auto stats = uc.GetStats();
  if (!Check(stats.curves_received == 1, "curves_received must be 1"))
    return false;
  if (!Check(stats.curves_saved == 0, "curves_saved must be 0 on failure"))
    return false;
  return true;
}

bool test_snapshot_with_depth() {
  FakeVenueStorage storage;
  cex::backtest::app::VenueReplayUseCases uc(&storage);

  auto snapshot = MakeSnapshot("binance", "BTC/USDT");
  // Add bid/ask depth levels
  auto* bp1 = snapshot.add_bid_prices();
  bp1->set_units(7000000); bp1->set_scale(2);
  auto* bq1 = snapshot.add_bid_quantities();
  bq1->set_units(100000); bq1->set_scale(6);
  auto* bp2 = snapshot.add_bid_prices();
  bp2->set_units(6999000); bp2->set_scale(2);
  auto* bq2 = snapshot.add_bid_quantities();
  bq2->set_units(200000); bq2->set_scale(6);

  auto* ap1 = snapshot.add_ask_prices();
  ap1->set_units(7001000); ap1->set_scale(2);
  auto* aq1 = snapshot.add_ask_quantities();
  aq1->set_units(150000); aq1->set_scale(6);

  uc.OnVenueSnapshot(snapshot);

  if (!Check(storage.save_snapshot_calls == 1, "must persist snapshot with depth"))
    return false;

  auto stats = uc.GetStats();
  if (!Check(stats.snapshots_saved == 1, "snapshots_saved must be 1"))
    return false;
  return true;
}

bool test_mixed_snapshot_and_curve_failures() {
  FakeVenueStorage storage;
  storage.fail_snapshot = true;
  cex::backtest::app::VenueReplayUseCases uc(&storage);

  uc.OnVenueSnapshot(MakeSnapshot("binance", "BTC/USDT"));
  uc.OnVenueLiquidityCurve(MakeCurve("binance", "BTC/USDT"));

  auto stats = uc.GetStats();
  if (!Check(stats.snapshots_received == 1, "snapshots_received must be 1"))
    return false;
  if (!Check(stats.snapshots_saved == 0, "snapshots_saved must be 0 (failed)"))
    return false;
  if (!Check(stats.curves_received == 1, "curves_received must be 1"))
    return false;
  if (!Check(stats.curves_saved == 1, "curves_saved must be 1 (curve storage OK)"))
    return false;
  return true;
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

  run("test_snapshot_persisted", test_snapshot_persisted);
  run("test_curve_persisted", test_curve_persisted);
  run("test_multiple_snapshots_and_curves", test_multiple_snapshots_and_curves);
  run("test_no_storage_configured", test_no_storage_configured);
  run("test_snapshot_storage_failure", test_snapshot_storage_failure);
  run("test_curve_storage_failure", test_curve_storage_failure);
  run("test_snapshot_with_depth", test_snapshot_with_depth);
  run("test_mixed_snapshot_and_curve_failures", test_mixed_snapshot_and_curve_failures);

  if (all_passed) {
    std::cout << "[OK] venue_replay_uc_test passed (8 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
