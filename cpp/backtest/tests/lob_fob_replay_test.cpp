#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "app/lob_fob_replay_uc.hpp"
#include "app/replay_metrics.hpp"

namespace {

using cex::backtest::app::CurveRow;
using cex::backtest::app::IReplayReader;
using cex::backtest::app::LobFobReplayUseCases;
using cex::backtest::app::ReplayConfig;
using cex::backtest::app::ReplayCurveComparison;
using cex::backtest::app::ReplayMetricsCalculator;
using cex::backtest::app::ReplayRunMetrics;
using cex::backtest::app::SnapshotRow;

// --- Fake reader ---

struct FakeReplayReader final : public IReplayReader {
  std::vector<SnapshotRow> snapshots;
  std::vector<CurveRow> curves;

  std::vector<SnapshotRow> LoadSnapshots(
      const std::string& /*venue_id*/,
      const std::string& /*symbol*/,
      int64_t /*from_ms*/,
      int64_t /*to_ms*/) override {
    return snapshots;
  }

  std::vector<CurveRow> LoadCurves(
      const std::string& /*venue_id*/,
      const std::string& /*symbol*/,
      int64_t /*from_ms*/,
      int64_t /*to_ms*/) override {
    return curves;
  }
};

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool CheckApprox(double actual, double expected, double tol,
                 const std::string& name) {
  if (std::fabs(actual - expected) <= tol) return true;
  std::cerr << "[FAIL] " << name << " expected " << expected
            << " got " << actual << std::endl;
  return false;
}

// Helper: make a snapshot with 3 bid and 3 ask levels.
SnapshotRow MakeSnapshot(int64_t event_time_ms) {
  SnapshotRow s;
  s.venue_id = "binance";
  s.symbol = "BTC/USDT";
  s.event_time_ms = event_time_ms;
  s.best_bid = 70000.0;
  s.best_ask = 70010.0;
  s.mid_price = 70005.0;
  s.spread = 10.0;
  s.tick_size = 0.01;
  s.lot_size = 0.001;
  // [[price, qty], ...] format
  s.bid_depth_json = "[[70000,1.0],[69990,2.0],[69980,1.5]]";
  s.ask_depth_json = "[[70010,1.0],[70020,2.0],[70030,1.5]]";
  return s;
}

// Helper: make a stored curve that matches what the pipeline would produce.
CurveRow MakeCurve(int64_t event_time_ms, const std::string& level) {
  CurveRow c;
  c.venue_id = "binance";
  c.symbol = "BTC/USDT";
  c.event_time_ms = event_time_ms;
  c.snapshot_id = "snap-1";
  c.level = level;
  // Approximate values matching the pipeline output for the test depth.
  c.bid_p_of_q = "[70000,70000,69990,69980]";
  c.bid_s_of_q = "[0,70000,209980,314950]";
  c.ask_p_of_q = "[70010,70010,70020,70030]";
  c.ask_s_of_q = "[0,70010,210050,315095]";
  c.bid_q_grid = "[0,1,3,4.5]";
  c.ask_q_grid = "[0,1,3,4.5]";
  c.epsilon1 = 0.001;
  c.epsilon2 = 0.002;
  c.epsilon3 = 0.0;
  c.confidence = 0.95;
  c.mid_price = 70005.0;
  c.tau_ms = 100.0;
  return c;
}

// --- Tests ---

bool test_replay_no_reader() {
  LobFobReplayUseCases uc(nullptr);
  ReplayConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 1000;
  cfg.to_ms = 2000;

  auto result = uc.RunReplay(cfg);
  if (!Check(result.empty(), "replay without reader must return empty")) return false;

  auto stats = uc.GetStats();
  if (!Check(stats.replays_run == 0, "replays_run must be 0 without reader")) return false;
  return true;
}

bool test_replay_empty_snapshots() {
  FakeReplayReader reader;
  // No snapshots loaded.
  LobFobReplayUseCases uc(&reader);
  ReplayConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 1000;
  cfg.to_ms = 2000;

  auto result = uc.RunReplay(cfg);
  if (!Check(result.empty(), "replay with no snapshots must return empty")) return false;

  auto stats = uc.GetStats();
  if (!Check(stats.replays_run == 1, "replays_run must be 1")) return false;
  if (!Check(stats.snapshots_processed == 0, "snapshots_processed must be 0")) return false;
  return true;
}

bool test_replay_single_snapshot_no_stored_curves() {
  FakeReplayReader reader;
  reader.snapshots.push_back(MakeSnapshot(1000));
  // No stored curves — comparisons will have matched=false.

  LobFobReplayUseCases uc(&reader);
  ReplayConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 1000;
  cfg.to_ms = 2000;
  cfg.tau_ms = 100.0;

  auto result = uc.RunReplay(cfg);
  // 3 levels (L1, L2, L3) per snapshot.
  if (!Check(result.size() == 3, "must produce 3 comparisons (L1/L2/L3)")) return false;
  if (!Check(result[0].level == "L1", "first must be L1")) return false;
  if (!Check(result[1].level == "L2", "second must be L2")) return false;
  if (!Check(result[2].level == "L3", "third must be L3")) return false;
  if (!Check(!result[0].matched, "L1 must be unmatched")) return false;
  if (!Check(!result[1].matched, "L2 must be unmatched")) return false;
  if (!Check(!result[2].matched, "L3 must be unmatched")) return false;

  auto stats = uc.GetStats();
  if (!Check(stats.snapshots_processed == 1, "snapshots_processed must be 1")) return false;
  if (!Check(stats.comparisons_produced == 3, "comparisons_produced must be 3")) return false;
  return true;
}

bool test_replay_single_snapshot_with_stored_curve() {
  FakeReplayReader reader;
  reader.snapshots.push_back(MakeSnapshot(1000));
  reader.curves.push_back(MakeCurve(1000, "L1"));

  LobFobReplayUseCases uc(&reader);
  ReplayConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 1000;
  cfg.to_ms = 2000;
  cfg.tau_ms = 100.0;

  auto result = uc.RunReplay(cfg);
  if (!Check(result.size() == 3, "must produce 3 comparisons")) return false;
  if (!Check(result[0].matched, "L1 must be matched")) return false;
  if (!Check(!result[1].matched, "L2 must be unmatched (no stored L2)")) return false;
  if (!Check(!result[2].matched, "L3 must be unmatched (no stored L3)")) return false;

  // MAE for matched L1 should be finite and non-negative.
  if (!Check(result[0].bid_p_of_q_mae >= 0, "L1 bid MAE must be >= 0")) return false;
  if (!Check(result[0].ask_p_of_q_mae >= 0, "L1 ask MAE must be >= 0")) return false;
  if (!Check(result[0].original_epsilon1 == 0.001, "original eps1 must match")) return false;
  if (!Check(result[0].original_confidence == 0.95, "original confidence must match")) return false;
  return true;
}

bool test_replay_multiple_snapshots() {
  FakeReplayReader reader;
  reader.snapshots.push_back(MakeSnapshot(1000));
  reader.snapshots.push_back(MakeSnapshot(2000));
  reader.snapshots.push_back(MakeSnapshot(3000));
  reader.curves.push_back(MakeCurve(1000, "L1"));
  reader.curves.push_back(MakeCurve(2000, "L1"));
  reader.curves.push_back(MakeCurve(2000, "L2"));

  LobFobReplayUseCases uc(&reader);
  ReplayConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 1000;
  cfg.to_ms = 3000;
  cfg.tau_ms = 100.0;

  auto result = uc.RunReplay(cfg);
  // 3 snapshots * 3 levels = 9 comparisons.
  if (!Check(result.size() == 9, "must produce 9 comparisons")) return false;

  // Count matched.
  int matched = 0;
  for (const auto& c : result) {
    if (c.matched) ++matched;
  }
  // snap 1000: L1 matched. snap 2000: L1 + L2 matched. snap 3000: none.
  if (!Check(matched == 3, "must have 3 matched comparisons")) return false;

  auto stats = uc.GetStats();
  if (!Check(stats.snapshots_processed == 3, "snapshots_processed must be 3")) return false;
  return true;
}

bool test_replay_with_metrics() {
  FakeReplayReader reader;
  reader.snapshots.push_back(MakeSnapshot(1000));
  reader.snapshots.push_back(MakeSnapshot(2000));
  reader.curves.push_back(MakeCurve(1000, "L1"));
  reader.curves.push_back(MakeCurve(2000, "L1"));

  LobFobReplayUseCases uc(&reader);
  ReplayConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 1000;
  cfg.to_ms = 2000;
  cfg.tau_ms = 100.0;

  auto metrics = uc.RunReplayWithMetrics(cfg);
  if (!Check(metrics.venue_id == "binance", "venue_id must be binance")) return false;
  if (!Check(metrics.symbol == "BTC/USDT", "symbol must be BTC/USDT")) return false;
  if (!Check(metrics.curves_compared == 6, "curves_compared must be 6")) return false;
  if (!Check(metrics.l1_count == 2, "l1_count must be 2")) return false;
  if (!Check(metrics.l2_count == 2, "l2_count must be 2")) return false;
  if (!Check(metrics.l3_count == 2, "l3_count must be 2")) return false;
  if (!Check(metrics.curves_matched == 2, "curves_matched must be 2")) return false;
  if (!Check(metrics.mean_confidence > 0, "mean_confidence must be > 0")) return false;
  return true;
}

bool test_replay_empty_depth_skipped() {
  FakeReplayReader reader;
  SnapshotRow empty_snap;
  empty_snap.venue_id = "binance";
  empty_snap.symbol = "BTC/USDT";
  empty_snap.event_time_ms = 1000;
  empty_snap.bid_depth_json = "[]";
  empty_snap.ask_depth_json = "[]";
  reader.snapshots.push_back(empty_snap);

  LobFobReplayUseCases uc(&reader);
  ReplayConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 1000;
  cfg.to_ms = 2000;

  auto result = uc.RunReplay(cfg);
  if (!Check(result.empty(), "empty depth snapshot must be skipped")) return false;
  return true;
}

bool test_compute_mae_basic() {
  std::vector<double> a = {1.0, 2.0, 3.0};
  std::vector<double> b = {1.0, 3.0, 5.0};
  double mae = ReplayMetricsCalculator::ComputeMAE(a, b);
  if (!CheckApprox(mae, 1.0, 1e-9, "MAE")) return false;
  return true;
}

bool test_compute_mae_empty() {
  std::vector<double> a;
  std::vector<double> b = {1.0};
  double mae = ReplayMetricsCalculator::ComputeMAE(a, b);
  if (!CheckApprox(mae, 0.0, 1e-9, "MAE of empty")) return false;
  return true;
}

bool test_compute_mae_different_lengths() {
  std::vector<double> a = {1.0, 2.0, 3.0, 4.0};
  std::vector<double> b = {2.0, 2.0};
  // Compares first 2 elements: |1-2| + |2-2| = 1, MAE = 0.5
  double mae = ReplayMetricsCalculator::ComputeMAE(a, b);
  if (!CheckApprox(mae, 0.5, 1e-9, "MAE different lengths")) return false;
  return true;
}

bool test_aggregate_metrics() {
  std::vector<ReplayCurveComparison> comps;
  {
    ReplayCurveComparison c;
    c.level = "L1";
    c.matched = true;
    c.bid_p_of_q_mae = 10.0;
    c.ask_p_of_q_mae = 20.0;
    c.original_epsilon1 = 0.001;
    c.original_epsilon2 = 0.002;
    c.original_epsilon3 = 0.0;
    c.original_confidence = 0.95;
    comps.push_back(c);
  }
  {
    ReplayCurveComparison c;
    c.level = "L1";
    c.matched = true;
    c.bid_p_of_q_mae = 30.0;
    c.ask_p_of_q_mae = 40.0;
    c.original_epsilon1 = 0.003;
    c.original_epsilon2 = 0.004;
    c.original_epsilon3 = 0.0;
    c.original_confidence = 0.85;
    comps.push_back(c);
  }
  {
    ReplayCurveComparison c;
    c.level = "L2";
    c.matched = false;
    c.bid_p_of_q_mae = 5.0;
    c.ask_p_of_q_mae = 6.0;
    comps.push_back(c);
  }

  auto m = ReplayMetricsCalculator::Aggregate("v", "s", 100, 200, comps);
  if (!Check(m.curves_compared == 3, "curves_compared")) return false;
  if (!Check(m.curves_matched == 2, "curves_matched")) return false;
  if (!Check(m.l1_count == 2, "l1_count")) return false;
  if (!Check(m.l2_count == 1, "l2_count")) return false;
  if (!CheckApprox(m.l1_bid_p_mae, 20.0, 1e-9, "l1_bid_p_mae avg")) return false;
  if (!CheckApprox(m.l1_ask_p_mae, 30.0, 1e-9, "l1_ask_p_mae avg")) return false;
  if (!CheckApprox(m.l2_bid_p_mae, 5.0, 1e-9, "l2_bid_p_mae")) return false;
  if (!CheckApprox(m.mean_epsilon1, 0.002, 1e-9, "mean_epsilon1")) return false;
  if (!CheckApprox(m.mean_confidence, 0.9, 1e-9, "mean_confidence")) return false;
  return true;
}

bool test_parse_depth_json() {
  // Test via running replay with known depth.
  FakeReplayReader reader;
  SnapshotRow s;
  s.venue_id = "test";
  s.symbol = "ETH/USDT";
  s.event_time_ms = 5000;
  s.bid_depth_json = "[[4000.0,10.0],[3990.0,5.0]]";
  s.ask_depth_json = "[[4010.0,8.0],[4020.0,3.0]]";
  s.tick_size = 0.01;
  s.lot_size = 0.001;
  reader.snapshots.push_back(s);

  LobFobReplayUseCases uc(&reader);
  ReplayConfig cfg;
  cfg.venue_id = "test";
  cfg.symbol = "ETH/USDT";
  cfg.from_ms = 5000;
  cfg.to_ms = 6000;
  cfg.tau_ms = 100.0;

  auto result = uc.RunReplay(cfg);
  // Should produce L1/L2/L3 comparisons (3 total).
  if (!Check(result.size() == 3, "must produce 3 comparisons for ETH snapshot")) return false;
  if (!Check(result[0].venue_id == "test", "venue_id must be test")) return false;
  if (!Check(result[0].symbol == "ETH/USDT", "symbol must be ETH/USDT")) return false;
  return true;
}

bool test_stats_accumulate() {
  FakeReplayReader reader;
  reader.snapshots.push_back(MakeSnapshot(1000));

  LobFobReplayUseCases uc(&reader);
  ReplayConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 1000;
  cfg.to_ms = 2000;
  cfg.tau_ms = 100.0;

  uc.RunReplay(cfg);
  uc.RunReplay(cfg);

  auto stats = uc.GetStats();
  if (!Check(stats.replays_run == 2, "replays_run must be 2")) return false;
  if (!Check(stats.snapshots_processed == 2, "snapshots_processed must be 2")) return false;
  if (!Check(stats.comparisons_produced == 6, "comparisons_produced must be 6")) return false;
  if (!Check(stats.last_venue_id == "binance", "last_venue_id")) return false;
  if (!Check(stats.last_symbol == "BTC/USDT", "last_symbol")) return false;
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

  run("test_replay_no_reader", test_replay_no_reader);
  run("test_replay_empty_snapshots", test_replay_empty_snapshots);
  run("test_replay_single_snapshot_no_stored_curves", test_replay_single_snapshot_no_stored_curves);
  run("test_replay_single_snapshot_with_stored_curve", test_replay_single_snapshot_with_stored_curve);
  run("test_replay_multiple_snapshots", test_replay_multiple_snapshots);
  run("test_replay_with_metrics", test_replay_with_metrics);
  run("test_replay_empty_depth_skipped", test_replay_empty_depth_skipped);
  run("test_compute_mae_basic", test_compute_mae_basic);
  run("test_compute_mae_empty", test_compute_mae_empty);
  run("test_compute_mae_different_lengths", test_compute_mae_different_lengths);
  run("test_aggregate_metrics", test_aggregate_metrics);
  run("test_parse_depth_json", test_parse_depth_json);
  run("test_stats_accumulate", test_stats_accumulate);

  if (all_passed) {
    std::cout << "[OK] lob_fob_replay_test passed (13 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
