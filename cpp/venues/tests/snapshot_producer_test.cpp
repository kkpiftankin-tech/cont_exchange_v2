#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "app/snapshot_producer.hpp"

namespace {

using cex::common::Decimal;
using cex::venues::app::IMessagePublisher;
using cex::venues::app::ISnapshotStorage;
using cex::venues::app::SnapshotProducer;
using cex::venues::app::SnapshotProducerConfig;
using cex::venues::domain::VenueBookLevel;
using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueRawSnapshot;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

// Fake publisher that captures messages.
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

// Fake storage that counts saves.
struct FakeStorage final : public ISnapshotStorage {
  int save_calls{0};
  bool fail{false};
  fob::venue::v1::VenueSnapshot last_snapshot;

  bool SaveSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) override {
    ++save_calls;
    last_snapshot = snapshot;
    return !fail;
  }
};

VenueRawSnapshot MakeRaw(uint64_t seq = 0) {
  VenueRawSnapshot raw;
  raw.venue_id = "binance";
  raw.instrument.set_symbol("BTC/USDT");
  raw.instrument.set_base("BTC");
  raw.instrument.set_quote("USDT");
  raw.timestamp.set_seconds(1700000000);
  raw.sequence = seq;
  raw.status = VenueConnectionStatus::kConnected;
  raw.best_bid = Decimal{70000, 0};
  raw.best_ask = Decimal{70010, 0};
  raw.bids = {VenueBookLevel{Decimal{70000, 0}, Decimal{1, 0}}};
  raw.asks = {VenueBookLevel{Decimal{70010, 0}, Decimal{1, 0}}};
  raw.fees.maker = Decimal{1, 4};
  raw.fees.taker = Decimal{2, 4};
  raw.trading_rules.tick_size = Decimal{1, 0};
  raw.trading_rules.lot_size = Decimal{1, 3};
  return raw;
}

SnapshotProducerConfig MakeConfig() {
  SnapshotProducerConfig cfg;
  cfg.normalization.fee_scale = 6;
  cfg.normalization.depth_config.max_levels_per_side = 20;
  cfg.status.stale_threshold_ms = 3000;
  cfg.topic = "venue.snapshots";
  return cfg;
}

// --- Tests ---

bool test_publish_basic() {
  FakePublisher pub;
  SnapshotProducer producer(&pub, nullptr, MakeConfig());
  bool ok = producer.Publish(MakeRaw(1));
  if (!Check(ok, "should publish")) return false;
  if (!Check(pub.messages.size() == 1, "1 message")) return false;
  if (!Check(pub.messages[0].topic == "venue.snapshots", "topic")) return false;

  auto stats = producer.GetStats();
  if (!Check(stats.published == 1, "published count")) return false;
  if (!Check(stats.duplicates_skipped == 0, "no duplicates")) return false;
  return true;
}

bool test_publish_null_publisher() {
  SnapshotProducer producer(nullptr, nullptr, MakeConfig());
  bool ok = producer.Publish(MakeRaw(1));
  return Check(!ok, "null publisher should return false");
}

bool test_dedup_same_sequence() {
  FakePublisher pub;
  SnapshotProducer producer(&pub, nullptr, MakeConfig());

  producer.Publish(MakeRaw(10));
  bool ok = producer.Publish(MakeRaw(10));

  if (!Check(!ok, "duplicate should return false")) return false;
  if (!Check(pub.messages.size() == 1, "only 1 message published")) return false;

  auto stats = producer.GetStats();
  if (!Check(stats.published == 1, "1 published")) return false;
  if (!Check(stats.duplicates_skipped == 1, "1 skipped")) return false;
  return true;
}

bool test_dedup_lower_sequence() {
  FakePublisher pub;
  SnapshotProducer producer(&pub, nullptr, MakeConfig());

  producer.Publish(MakeRaw(10));
  bool ok = producer.Publish(MakeRaw(5));  // older sequence

  if (!Check(!ok, "older sequence should be deduped")) return false;
  if (!Check(pub.messages.size() == 1, "only 1 message")) return false;
  return true;
}

bool test_dedup_higher_sequence_passes() {
  FakePublisher pub;
  SnapshotProducer producer(&pub, nullptr, MakeConfig());

  producer.Publish(MakeRaw(10));
  bool ok = producer.Publish(MakeRaw(11));

  if (!Check(ok, "higher sequence should pass")) return false;
  if (!Check(pub.messages.size() == 2, "2 messages")) return false;
  return true;
}

bool test_dedup_zero_sequence_no_dedup() {
  FakePublisher pub;
  SnapshotProducer producer(&pub, nullptr, MakeConfig());

  producer.Publish(MakeRaw(0));
  bool ok = producer.Publish(MakeRaw(0));

  if (!Check(ok, "seq=0 should not dedup")) return false;
  if (!Check(pub.messages.size() == 2, "2 messages with seq=0")) return false;
  return true;
}

bool test_dedup_different_symbols() {
  FakePublisher pub;
  SnapshotProducer producer(&pub, nullptr, MakeConfig());

  auto raw1 = MakeRaw(10);
  auto raw2 = MakeRaw(10);
  raw2.instrument.set_symbol("ETH/USDT");

  producer.Publish(raw1);
  bool ok = producer.Publish(raw2);

  if (!Check(ok, "different symbol same seq should pass")) return false;
  if (!Check(pub.messages.size() == 2, "2 messages for different symbols")) return false;
  return true;
}

bool test_storage_called() {
  FakePublisher pub;
  FakeStorage storage;
  SnapshotProducer producer(&pub, &storage, MakeConfig());

  producer.Publish(MakeRaw(1));

  if (!Check(storage.save_calls == 1, "storage should be called once")) return false;
  if (!Check(storage.last_snapshot.venue_id() == "binance", "stored venue_id")) return false;

  auto stats = producer.GetStats();
  if (!Check(stats.storage_writes == 1, "1 storage write")) return false;
  return true;
}

bool test_storage_failure_counted() {
  FakePublisher pub;
  FakeStorage storage;
  storage.fail = true;
  SnapshotProducer producer(&pub, &storage, MakeConfig());

  producer.Publish(MakeRaw(1));

  if (!Check(storage.save_calls == 1, "storage attempted")) return false;

  auto stats = producer.GetStats();
  if (!Check(stats.storage_failures == 1, "1 storage failure")) return false;
  if (!Check(stats.storage_writes == 0, "0 successful writes")) return false;
  return true;
}

bool test_storage_not_called_on_dedup() {
  FakePublisher pub;
  FakeStorage storage;
  SnapshotProducer producer(&pub, &storage, MakeConfig());

  producer.Publish(MakeRaw(10));
  producer.Publish(MakeRaw(10));  // duplicate

  if (!Check(storage.save_calls == 1, "storage only called once")) return false;
  return true;
}

bool test_status_derived_in_output() {
  FakePublisher pub;
  FakeStorage storage;
  SnapshotProducer producer(&pub, &storage, MakeConfig());

  auto raw = MakeRaw(1);
  raw.status = VenueConnectionStatus::kConnected;
  producer.Publish(raw);

  if (!Check(storage.last_snapshot.status() == "connected", "status=connected")) return false;
  return true;
}

bool test_status_empty_book_derived() {
  FakePublisher pub;
  FakeStorage storage;
  SnapshotProducer producer(&pub, &storage, MakeConfig());

  auto raw = MakeRaw(1);
  raw.bids.clear();
  raw.asks.clear();
  producer.Publish(raw);

  if (!Check(storage.last_snapshot.status() == "empty", "empty book -> status=empty")) return false;
  return true;
}

bool test_status_disconnected_derived() {
  FakePublisher pub;
  FakeStorage storage;
  SnapshotProducer producer(&pub, &storage, MakeConfig());

  auto raw = MakeRaw(1);
  raw.status = VenueConnectionStatus::kDisconnected;
  producer.Publish(raw);

  if (!Check(storage.last_snapshot.status() == "disconnected",
             "disconnected -> status=disconnected")) return false;
  return true;
}

bool test_multiple_publishes_accumulate_stats() {
  FakePublisher pub;
  FakeStorage storage;
  SnapshotProducer producer(&pub, &storage, MakeConfig());

  producer.Publish(MakeRaw(1));
  producer.Publish(MakeRaw(2));
  producer.Publish(MakeRaw(3));
  producer.Publish(MakeRaw(3));  // dup

  auto stats = producer.GetStats();
  if (!Check(stats.published == 3, "3 published")) return false;
  if (!Check(stats.duplicates_skipped == 1, "1 skipped")) return false;
  if (!Check(stats.storage_writes == 3, "3 storage writes")) return false;
  return true;
}

bool test_partition_key_format() {
  FakePublisher pub;
  SnapshotProducer producer(&pub, nullptr, MakeConfig());

  producer.Publish(MakeRaw(1));

  if (!Check(pub.messages[0].key == "binance|BTC/USDT",
             "partition key format")) return false;
  return true;
}

bool test_stale_threshold_override_is_applied_per_publish() {
  FakePublisher pub;
  FakeStorage storage;
  SnapshotProducer producer(&pub, &storage, MakeConfig());

  if (!Check(producer.Publish(MakeRaw(1)), "first snapshot must publish")) return false;
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  SnapshotProducer::PublishOptions options;
  options.stale_threshold_ms = 1;
  if (!Check(producer.Publish(MakeRaw(2), nullptr, options),
             "second snapshot with override must publish")) {
    return false;
  }
  return Check(storage.last_snapshot.status() == "stale",
               "per-publish stale threshold override must affect derived status");
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

  run("test_publish_basic", test_publish_basic);
  run("test_publish_null_publisher", test_publish_null_publisher);
  run("test_dedup_same_sequence", test_dedup_same_sequence);
  run("test_dedup_lower_sequence", test_dedup_lower_sequence);
  run("test_dedup_higher_sequence_passes", test_dedup_higher_sequence_passes);
  run("test_dedup_zero_sequence_no_dedup", test_dedup_zero_sequence_no_dedup);
  run("test_dedup_different_symbols", test_dedup_different_symbols);
  run("test_storage_called", test_storage_called);
  run("test_storage_failure_counted", test_storage_failure_counted);
  run("test_storage_not_called_on_dedup", test_storage_not_called_on_dedup);
  run("test_status_derived_in_output", test_status_derived_in_output);
  run("test_status_empty_book_derived", test_status_empty_book_derived);
  run("test_status_disconnected_derived", test_status_disconnected_derived);
  run("test_multiple_publishes_accumulate_stats", test_multiple_publishes_accumulate_stats);
  run("test_partition_key_format", test_partition_key_format);
  run("test_stale_threshold_override_is_applied_per_publish",
      test_stale_threshold_override_is_applied_per_publish);

  if (all_passed) {
    std::cout << "[OK] snapshot_producer_test passed (16 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
