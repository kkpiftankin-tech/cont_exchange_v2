#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "domain/snapshot_status.hpp"
#include "domain/venue_adapter.hpp"

namespace {

using cex::common::Decimal;
using cex::venues::domain::DeriveSnapshotStatus;
using cex::venues::domain::SnapshotAgeTracker;
using cex::venues::domain::SnapshotStatusConfig;
using cex::venues::domain::VenueBookLevel;
using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueRawSnapshot;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

VenueRawSnapshot MakeRaw(VenueConnectionStatus status = VenueConnectionStatus::kConnected) {
  VenueRawSnapshot raw;
  raw.venue_id = "binance";
  raw.instrument.set_symbol("BTC/USDT");
  raw.status = status;
  raw.bids = {VenueBookLevel{Decimal{70000, 0}, Decimal{1, 0}}};
  raw.asks = {VenueBookLevel{Decimal{70010, 0}, Decimal{1, 0}}};
  return raw;
}

// --- DeriveSnapshotStatus tests ---

bool test_status_connected() {
  auto raw = MakeRaw();
  SnapshotStatusConfig cfg;
  cfg.stale_threshold_ms = 3000;
  auto status = DeriveSnapshotStatus(raw, 500, cfg);
  return Check(status == VenueConnectionStatus::kConnected,
               "should be connected when age < threshold");
}

bool test_status_disconnected_overrides() {
  auto raw = MakeRaw(VenueConnectionStatus::kDisconnected);
  SnapshotStatusConfig cfg;
  auto status = DeriveSnapshotStatus(raw, 0, cfg);
  return Check(status == VenueConnectionStatus::kDisconnected,
               "disconnected should override everything");
}

bool test_status_empty_book() {
  auto raw = MakeRaw();
  raw.bids.clear();
  raw.asks.clear();
  SnapshotStatusConfig cfg;
  auto status = DeriveSnapshotStatus(raw, 0, cfg);
  return Check(status == VenueConnectionStatus::kEmpty,
               "empty book should be kEmpty");
}

bool test_status_stale_by_age() {
  auto raw = MakeRaw();
  SnapshotStatusConfig cfg;
  cfg.stale_threshold_ms = 3000;
  auto status = DeriveSnapshotStatus(raw, 5000, cfg);
  return Check(status == VenueConnectionStatus::kStale,
               "should be stale when age > threshold");
}

bool test_status_stale_exact_threshold() {
  auto raw = MakeRaw();
  SnapshotStatusConfig cfg;
  cfg.stale_threshold_ms = 3000;
  // Exactly at threshold should NOT be stale (> not >=).
  auto status = DeriveSnapshotStatus(raw, 3000, cfg);
  return Check(status == VenueConnectionStatus::kConnected,
               "exactly at threshold should be connected");
}

bool test_status_disconnected_beats_empty() {
  auto raw = MakeRaw(VenueConnectionStatus::kDisconnected);
  raw.bids.clear();
  raw.asks.clear();
  SnapshotStatusConfig cfg;
  auto status = DeriveSnapshotStatus(raw, 0, cfg);
  return Check(status == VenueConnectionStatus::kDisconnected,
               "disconnected should beat empty");
}

bool test_status_empty_beats_stale() {
  auto raw = MakeRaw();
  raw.bids.clear();
  raw.asks.clear();
  SnapshotStatusConfig cfg;
  cfg.stale_threshold_ms = 1000;
  auto status = DeriveSnapshotStatus(raw, 5000, cfg);
  return Check(status == VenueConnectionStatus::kEmpty,
               "empty should beat stale");
}

bool test_status_one_side_empty_not_empty() {
  auto raw = MakeRaw();
  raw.asks.clear();  // Only bids present.
  SnapshotStatusConfig cfg;
  auto status = DeriveSnapshotStatus(raw, 0, cfg);
  // Only BOTH sides empty triggers kEmpty.
  return Check(status == VenueConnectionStatus::kConnected,
               "one side empty should not be kEmpty");
}

// --- SnapshotAgeTracker tests ---

bool test_age_tracker_first_call() {
  SnapshotAgeTracker tracker;
  int64_t age = tracker.RecordAndGetAge("binance", "BTC/USDT");
  return Check(age == 0, "first call should return 0");
}

bool test_age_tracker_second_call() {
  SnapshotAgeTracker tracker;
  tracker.RecordAndGetAge("binance", "BTC/USDT");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  int64_t age = tracker.RecordAndGetAge("binance", "BTC/USDT");
  return Check(age >= 40 && age <= 200,
               "second call age should be ~50ms");
}

bool test_age_tracker_different_keys() {
  SnapshotAgeTracker tracker;
  tracker.RecordAndGetAge("binance", "BTC/USDT");
  int64_t age = tracker.RecordAndGetAge("coinbase", "ETH/USDT");
  return Check(age == 0, "different key should return 0");
}

bool test_age_tracker_get_age_unknown() {
  SnapshotAgeTracker tracker;
  int64_t age = tracker.GetAge("unknown", "X/Y");
  return Check(age == -1, "unknown key should return -1");
}

bool test_age_tracker_get_age_known() {
  SnapshotAgeTracker tracker;
  tracker.RecordAndGetAge("binance", "BTC/USDT");
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  int64_t age = tracker.GetAge("binance", "BTC/USDT");
  return Check(age >= 20 && age <= 200,
               "get_age should return elapsed time");
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

  run("test_status_connected", test_status_connected);
  run("test_status_disconnected_overrides", test_status_disconnected_overrides);
  run("test_status_empty_book", test_status_empty_book);
  run("test_status_stale_by_age", test_status_stale_by_age);
  run("test_status_stale_exact_threshold", test_status_stale_exact_threshold);
  run("test_status_disconnected_beats_empty", test_status_disconnected_beats_empty);
  run("test_status_empty_beats_stale", test_status_empty_beats_stale);
  run("test_status_one_side_empty_not_empty", test_status_one_side_empty_not_empty);
  run("test_age_tracker_first_call", test_age_tracker_first_call);
  run("test_age_tracker_second_call", test_age_tracker_second_call);
  run("test_age_tracker_different_keys", test_age_tracker_different_keys);
  run("test_age_tracker_get_age_unknown", test_age_tracker_get_age_unknown);
  run("test_age_tracker_get_age_known", test_age_tracker_get_age_known);

  if (all_passed) {
    std::cout << "[OK] snapshot_status_test passed (13 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
