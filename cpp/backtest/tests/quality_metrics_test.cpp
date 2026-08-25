#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "app/quality_metrics.hpp"
#include "app/quality_metrics_uc.hpp"

namespace {

using cex::backtest::app::CurveRow;
using cex::backtest::app::DistributionStats;
using cex::backtest::app::IQualityMetricsStorage;
using cex::backtest::app::IReplayReader;
using cex::backtest::app::QualityConfig;
using cex::backtest::app::QualityMetricsCalculator;
using cex::backtest::app::QualityMetricsUseCases;
using cex::backtest::app::SnapshotRow;
using cex::backtest::app::VenueQualityReport;

// --- Fakes ---

struct FakeReplayReader final : public IReplayReader {
  std::vector<SnapshotRow> snapshots;
  std::vector<CurveRow> curves;

  std::vector<SnapshotRow> LoadSnapshots(
      const std::string&, const std::string&, int64_t, int64_t) override {
    return snapshots;
  }

  std::vector<CurveRow> LoadCurves(
      const std::string&, const std::string&, int64_t, int64_t) override {
    return curves;
  }
};

struct FakeQualityStorage final : public IQualityMetricsStorage {
  int save_calls{0};
  VenueQualityReport last_report;
  bool fail{false};

  bool SaveQualityReport(const VenueQualityReport& report) override {
    ++save_calls;
    last_report = report;
    return !fail;
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

SnapshotRow MakeSnap(int64_t ts, const std::string& status = "connected") {
  SnapshotRow s;
  s.venue_id = "binance";
  s.symbol = "BTC/USDT";
  s.event_time_ms = ts;
  s.status = status;
  s.best_bid = 70000;
  s.best_ask = 70010;
  return s;
}

CurveRow MakeCurve(int64_t ts, const std::string& level,
                   double eps1, double eps2, double eps3, double conf) {
  CurveRow c;
  c.venue_id = "binance";
  c.symbol = "BTC/USDT";
  c.event_time_ms = ts;
  c.level = level;
  c.epsilon1 = eps1;
  c.epsilon2 = eps2;
  c.epsilon3 = eps3;
  c.confidence = conf;
  c.tau_ms = 100;
  return c;
}

// --- Tests ---

bool test_compute_stats_empty() {
  auto stats = QualityMetricsCalculator::ComputeStats({});
  if (!Check(stats.count == 0, "empty count must be 0")) return false;
  if (!CheckApprox(stats.mean, 0.0, 1e-9, "empty mean")) return false;
  return true;
}

bool test_compute_stats_single() {
  auto stats = QualityMetricsCalculator::ComputeStats({5.0});
  if (!Check(stats.count == 1, "single count")) return false;
  if (!CheckApprox(stats.mean, 5.0, 1e-9, "single mean")) return false;
  if (!CheckApprox(stats.median, 5.0, 1e-9, "single median")) return false;
  if (!CheckApprox(stats.p95, 5.0, 1e-9, "single p95")) return false;
  if (!CheckApprox(stats.p99, 5.0, 1e-9, "single p99")) return false;
  return true;
}

bool test_compute_stats_multiple() {
  // Values: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
  std::vector<double> vals = {5, 3, 1, 8, 10, 2, 7, 4, 9, 6};
  auto stats = QualityMetricsCalculator::ComputeStats(vals);
  if (!Check(stats.count == 10, "count must be 10")) return false;
  if (!CheckApprox(stats.mean, 5.5, 1e-9, "mean of 1..10")) return false;
  // Median of 1..10 = (5+6)/2 = 5.5
  if (!CheckApprox(stats.median, 5.5, 1e-9, "median of 1..10")) return false;
  // p95 of 1..10: index = 0.95 * 9 = 8.55 -> lerp(9, 10, 0.55) = 9.55
  if (!CheckApprox(stats.p95, 9.55, 1e-9, "p95 of 1..10")) return false;
  // p99: index = 0.99 * 9 = 8.91 -> lerp(9, 10, 0.91) = 9.91
  if (!CheckApprox(stats.p99, 9.91, 1e-9, "p99 of 1..10")) return false;
  return true;
}

bool test_percentile_basic() {
  std::vector<double> sorted = {1, 2, 3, 4, 5};
  if (!CheckApprox(QualityMetricsCalculator::Percentile(sorted, 0.0), 1.0, 1e-9, "p0")) return false;
  if (!CheckApprox(QualityMetricsCalculator::Percentile(sorted, 0.5), 3.0, 1e-9, "p50")) return false;
  if (!CheckApprox(QualityMetricsCalculator::Percentile(sorted, 1.0), 5.0, 1e-9, "p100")) return false;
  return true;
}

bool test_stale_rate_no_curves() {
  // 10s window, 1s interval, 0 curves -> stale rate = 1.0
  double rate = QualityMetricsCalculator::ComputeStaleRate(0, 10000, 1000, 0);
  if (!CheckApprox(rate, 1.0, 1e-9, "stale rate no curves")) return false;
  return true;
}

bool test_stale_rate_full() {
  // 10s window, 1s interval, 10 curves -> stale rate = 0.0
  double rate = QualityMetricsCalculator::ComputeStaleRate(0, 10000, 1000, 10);
  if (!CheckApprox(rate, 0.0, 1e-9, "stale rate full")) return false;
  return true;
}

bool test_stale_rate_partial() {
  // 10s window, 1s interval, 6 curves -> stale rate = 0.4
  double rate = QualityMetricsCalculator::ComputeStaleRate(0, 10000, 1000, 6);
  if (!CheckApprox(rate, 0.4, 1e-9, "stale rate partial")) return false;
  return true;
}

bool test_stale_rate_zero_interval() {
  double rate = QualityMetricsCalculator::ComputeStaleRate(0, 10000, 0, 5);
  if (!CheckApprox(rate, 0.0, 1e-9, "stale rate zero interval")) return false;
  return true;
}

bool test_build_report_basic() {
  std::vector<CurveRow> curves = {
      MakeCurve(1000, "L1", 0.001, 0.002, 0.0, 0.95),
      MakeCurve(2000, "L1", 0.003, 0.004, 0.0, 0.90),
      MakeCurve(3000, "L2", 0.002, 0.003, 0.001, 0.92),
  };
  std::vector<SnapshotRow> snaps = {
      MakeSnap(1000, "connected"),
      MakeSnap(2000, "connected"),
      MakeSnap(3000, "stale"),
      MakeSnap(4000, "connected"),
  };

  auto r = QualityMetricsCalculator::BuildReport(
      "binance", "BTC/USDT", 0, 10000, curves, snaps, 1000.0);

  if (!Check(r.venue_id == "binance", "venue_id")) return false;
  if (!Check(r.total_curves == 3, "total_curves")) return false;
  if (!Check(r.total_snapshots == 4, "total_snapshots")) return false;
  if (!Check(r.l1_count == 2, "l1_count")) return false;
  if (!Check(r.l2_count == 1, "l2_count")) return false;
  if (!Check(r.l3_count == 0, "l3_count")) return false;

  // Epsilon1: {0.001, 0.003, 0.002} -> mean = 0.002
  if (!CheckApprox(r.epsilon1.mean, 0.002, 1e-9, "eps1 mean")) return false;
  if (!Check(r.epsilon1.count == 3, "eps1 count")) return false;

  // Uptime: 3/4 connected
  if (!CheckApprox(r.uptime, 0.75, 1e-9, "uptime")) return false;
  if (!Check(r.connected_snapshots == 3, "connected_snapshots")) return false;

  // Stale rate: 10s / 1s = 10 expected, 3 actual -> (10-3)/10 = 0.7
  if (!CheckApprox(r.stale_rate, 0.7, 1e-9, "stale_rate")) return false;

  return true;
}

bool test_build_report_empty() {
  auto r = QualityMetricsCalculator::BuildReport(
      "binance", "BTC/USDT", 0, 10000, {}, {}, 1000.0);
  if (!Check(r.total_curves == 0, "empty total_curves")) return false;
  if (!Check(r.total_snapshots == 0, "empty total_snapshots")) return false;
  if (!CheckApprox(r.stale_rate, 1.0, 1e-9, "empty stale_rate")) return false;
  if (!CheckApprox(r.uptime, 0.0, 1e-9, "empty uptime")) return false;
  return true;
}

bool test_uc_compute_report() {
  FakeReplayReader reader;
  reader.curves = {
      MakeCurve(1000, "L1", 0.001, 0.002, 0.0, 0.95),
      MakeCurve(2000, "L2", 0.002, 0.003, 0.001, 0.90),
  };
  reader.snapshots = {
      MakeSnap(1000, "connected"),
      MakeSnap(2000, "connected"),
  };

  QualityMetricsUseCases uc(&reader, nullptr);
  QualityConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 0;
  cfg.to_ms = 5000;
  cfg.expected_interval_ms = 1000;

  auto report = uc.ComputeReport(cfg);
  if (!Check(report.total_curves == 2, "uc total_curves")) return false;
  if (!Check(report.total_snapshots == 2, "uc total_snapshots")) return false;
  if (!CheckApprox(report.uptime, 1.0, 1e-9, "uc uptime")) return false;

  auto stats = uc.GetStats();
  if (!Check(stats.reports_computed == 1, "reports_computed")) return false;
  if (!Check(stats.last_venue_id == "binance", "last_venue_id")) return false;
  return true;
}

bool test_uc_compute_and_save() {
  FakeReplayReader reader;
  reader.curves = {MakeCurve(1000, "L1", 0.001, 0.002, 0.0, 0.95)};
  reader.snapshots = {MakeSnap(1000)};

  FakeQualityStorage storage;
  QualityMetricsUseCases uc(&reader, &storage);

  QualityConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 0;
  cfg.to_ms = 5000;
  cfg.expected_interval_ms = 1000;

  auto report = uc.ComputeAndSaveReport(cfg);
  if (!Check(storage.save_calls == 1, "save must be called once")) return false;
  if (!Check(storage.last_report.venue_id == "binance", "saved venue_id")) return false;
  if (!Check(storage.last_report.total_curves == 1, "saved total_curves")) return false;

  auto stats = uc.GetStats();
  if (!Check(stats.reports_computed == 1, "reports_computed")) return false;
  if (!Check(stats.reports_saved == 1, "reports_saved")) return false;
  return true;
}

bool test_uc_save_failure() {
  FakeReplayReader reader;
  reader.curves = {MakeCurve(1000, "L1", 0.001, 0.002, 0.0, 0.95)};
  reader.snapshots = {MakeSnap(1000)};

  FakeQualityStorage storage;
  storage.fail = true;
  QualityMetricsUseCases uc(&reader, &storage);

  QualityConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 0;
  cfg.to_ms = 5000;
  cfg.expected_interval_ms = 1000;

  uc.ComputeAndSaveReport(cfg);
  if (!Check(storage.save_calls == 1, "save must be attempted")) return false;

  auto stats = uc.GetStats();
  if (!Check(stats.reports_computed == 1, "reports_computed after failure")) return false;
  if (!Check(stats.reports_saved == 0, "reports_saved must be 0 on failure")) return false;
  return true;
}

bool test_uc_no_reader() {
  QualityMetricsUseCases uc(nullptr, nullptr);

  QualityConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 0;
  cfg.to_ms = 5000;

  auto report = uc.ComputeReport(cfg);
  if (!Check(report.total_curves == 0, "no-reader total_curves")) return false;

  auto stats = uc.GetStats();
  if (!Check(stats.reports_computed == 0, "no-reader reports_computed must be 0")) return false;
  return true;
}

bool test_uc_multiple_runs_accumulate_stats() {
  FakeReplayReader reader;
  reader.curves = {MakeCurve(1000, "L1", 0.001, 0.002, 0.0, 0.95)};
  reader.snapshots = {MakeSnap(1000)};

  FakeQualityStorage storage;
  QualityMetricsUseCases uc(&reader, &storage);

  QualityConfig cfg;
  cfg.venue_id = "binance";
  cfg.symbol = "BTC/USDT";
  cfg.from_ms = 0;
  cfg.to_ms = 5000;
  cfg.expected_interval_ms = 1000;

  uc.ComputeAndSaveReport(cfg);
  uc.ComputeAndSaveReport(cfg);
  uc.ComputeReport(cfg);

  auto stats = uc.GetStats();
  if (!Check(stats.reports_computed == 3, "3 reports computed")) return false;
  if (!Check(stats.reports_saved == 2, "2 reports saved")) return false;
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

  run("test_compute_stats_empty", test_compute_stats_empty);
  run("test_compute_stats_single", test_compute_stats_single);
  run("test_compute_stats_multiple", test_compute_stats_multiple);
  run("test_percentile_basic", test_percentile_basic);
  run("test_stale_rate_no_curves", test_stale_rate_no_curves);
  run("test_stale_rate_full", test_stale_rate_full);
  run("test_stale_rate_partial", test_stale_rate_partial);
  run("test_stale_rate_zero_interval", test_stale_rate_zero_interval);
  run("test_build_report_basic", test_build_report_basic);
  run("test_build_report_empty", test_build_report_empty);
  run("test_uc_compute_report", test_uc_compute_report);
  run("test_uc_compute_and_save", test_uc_compute_and_save);
  run("test_uc_save_failure", test_uc_save_failure);
  run("test_uc_no_reader", test_uc_no_reader);
  run("test_uc_multiple_runs_accumulate_stats", test_uc_multiple_runs_accumulate_stats);

  if (all_passed) {
    std::cout << "[OK] quality_metrics_test passed (15 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
