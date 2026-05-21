#include "app/snapshot_producer.hpp"

#include <chrono>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"

namespace cex::venues::app {

namespace {

std::string decimal_or_zero(const fob::common::v1::Decimal& value) {
  return cex::common::Decimal::from_proto(value).to_string();
}

}  // namespace

SnapshotProducer::SnapshotProducer(IMessagePublisher* publisher,
                                   ISnapshotStorage* storage,
                                   const SnapshotProducerConfig& config)
    : publisher_(publisher), storage_(storage), config_(config) {}

bool SnapshotProducer::Publish(const domain::VenueRawSnapshot& raw,
                               fob::venue::v1::VenueSnapshot* normalized_snapshot_out,
                               const PublishOptions& options) {
  const auto started_at = std::chrono::steady_clock::now();
  if (publisher_ == nullptr) return false;

  const std::string& venue_id = raw.venue_id;
  const std::string symbol = raw.instrument.symbol();

  // F11-NORM-4: Sequence-based dedup.
  if (IsDuplicate(venue_id, symbol, raw.sequence)) {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.duplicates_skipped;
    cex::common::log_json("INFO", "Skipped duplicate raw venue snapshot",
                          {{"service", "venues"},
                           {"component", "venue_market_data_normalizer"},
                           {"participant", "Venue Market Data Normalizer"},
                           {"stage", "dedup_snapshot"},
                           {"topic", config_.topic},
                           {"venue", venue_id},
                           {"symbol", symbol},
                           {"sequence", std::to_string(raw.sequence)},
                           {"duplicates_skipped", std::to_string(stats_.duplicates_skipped)},
                           {"source_file", "cpp/venues/src/app/snapshot_producer.cpp"}});
    return false;
  }

  // F11-NORM-3: Derive status with age tracking.
  const int64_t age_ms = age_tracker_.RecordAndGetAge(venue_id, symbol);
  domain::SnapshotStatusConfig status_config = config_.status;
  if (options.stale_threshold_ms.has_value()) {
    status_config.stale_threshold_ms = std::max<int64_t>(1, *options.stale_threshold_ms);
  }
  const domain::VenueConnectionStatus derived_status =
      domain::DeriveSnapshotStatus(raw, age_ms, status_config);

  // Apply derived status to a mutable copy.
  domain::VenueRawSnapshot enriched = raw;
  enriched.status = derived_status;

  // F11-NORM-1/2: Normalize.
  auto snapshot = domain::NormalizeSnapshot(enriched, config_.normalization);
  if (normalized_snapshot_out != nullptr) {
    *normalized_snapshot_out = snapshot;
  }

  cex::common::log_json("INFO", "Normalized raw venue snapshot",
                        {{"service", "venues"},
                         {"component", "venue_market_data_normalizer"},
                         {"participant", "Venue Market Data Normalizer"},
                         {"stage", "normalize_snapshot"},
                         {"topic", config_.topic},
                         {"venue", snapshot.venue_id()},
                         {"symbol", snapshot.instrument().symbol()},
                         {"status", snapshot.status()},
                         {"sequence", std::to_string(raw.sequence)},
                         {"age_ms", std::to_string(age_ms)},
                         {"bid_levels", std::to_string(snapshot.bid_prices_size())},
                         {"ask_levels", std::to_string(snapshot.ask_prices_size())},
                         {"best_bid",
                          snapshot.has_best_bid() ? decimal_or_zero(snapshot.best_bid()) : "0"},
                         {"best_ask",
                          snapshot.has_best_ask() ? decimal_or_zero(snapshot.best_ask()) : "0"},
                         {"mid_price",
                          snapshot.has_mid_price() ? decimal_or_zero(snapshot.mid_price()) : "0"},
                         {"spread",
                          snapshot.has_spread() ? decimal_or_zero(snapshot.spread()) : "0"},
                         {"maker_fee",
                          snapshot.has_maker_fee() ? decimal_or_zero(snapshot.maker_fee()) : "0"},
                         {"taker_fee",
                          snapshot.has_taker_fee() ? decimal_or_zero(snapshot.taker_fee()) : "0"},
                         {"tick_size",
                          snapshot.has_tick_size() ? decimal_or_zero(snapshot.tick_size()) : "0"},
                         {"lot_size",
                          snapshot.has_lot_size() ? decimal_or_zero(snapshot.lot_size()) : "0"},
                         {"volume_24h",
                          snapshot.has_volume_24h() ? decimal_or_zero(snapshot.volume_24h()) : "0"},
                         {"source_file", "cpp/venues/src/app/snapshot_producer.cpp"}});

  // Publish to Kafka.
  const std::string key = snapshot.meta().partition_key();
  const std::string payload = cex::common::to_bytes(snapshot);
  const bool published = publisher_->Publish(config_.topic, key, payload);

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (published) {
      ++stats_.published;
    }
  }

  // F11-NORM-5: Persist to storage if available.
  bool storage_saved = false;
  if (storage_ != nullptr) {
    if (storage_->SaveSnapshot(snapshot)) {
      storage_saved = true;
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.storage_writes;
    } else {
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.storage_failures;
      cex::common::log_json("ERROR", "Failed to store normalized venue snapshot",
                            {{"service", "venues"},
                             {"component", "venue_market_data_normalizer"},
                             {"participant", "Venue Market Data Normalizer"},
                             {"stage", "store_snapshot"},
                             {"topic", config_.topic},
                             {"venue", snapshot.venue_id()},
                             {"symbol", snapshot.instrument().symbol()},
                             {"source_file",
                              "cpp/venues/src/app/snapshot_producer.cpp"}});
    }
  }

  const double snapshot_latency_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
          std::chrono::steady_clock::now() - started_at)
          .count();
  cex::common::log_json(published ? "INFO" : "ERROR", "Published venue.snapshot",
                        {{"service", "venues"},
                         {"component", "venue_market_data_normalizer"},
                         {"participant", "Venue Market Data Normalizer"},
                         {"stage", "publish_snapshot"},
                         {"topic", config_.topic},
                         {"event_id", snapshot.meta().event_id()},
                         {"correlation_id", snapshot.meta().correlation_id()},
                         {"venue", snapshot.venue_id()},
                         {"symbol", snapshot.instrument().symbol()},
                         {"status", snapshot.status()},
                         {"sequence", std::to_string(raw.sequence)},
                         {"age_ms", std::to_string(age_ms)},
                         {"bid_levels", std::to_string(snapshot.bid_prices_size())},
                         {"ask_levels", std::to_string(snapshot.ask_prices_size())},
                         {"best_bid",
                          snapshot.has_best_bid() ? decimal_or_zero(snapshot.best_bid()) : "0"},
                         {"best_ask",
                          snapshot.has_best_ask() ? decimal_or_zero(snapshot.best_ask()) : "0"},
                         {"mid_price",
                          snapshot.has_mid_price() ? decimal_or_zero(snapshot.mid_price()) : "0"},
                         {"spread",
                          snapshot.has_spread() ? decimal_or_zero(snapshot.spread()) : "0"},
                         {"volume_24h",
                          snapshot.has_volume_24h() ? decimal_or_zero(snapshot.volume_24h()) : "0"},
                         {"payload_bytes", std::to_string(payload.size())},
                         {"storage_saved", storage_saved ? "true" : "false"},
                         {"snapshot_latency_ms", std::to_string(snapshot_latency_ms)},
                         {"source_file", "cpp/venues/src/app/snapshot_producer.cpp"}});

  return published;
}

bool SnapshotProducer::IsDuplicate(const std::string& venue_id,
                                   const std::string& symbol,
                                   uint64_t sequence) {
  if (sequence == 0) return false;  // sequence 0 means "no dedup"

  const std::string key = venue_id + "|" + symbol;
  std::lock_guard<std::mutex> lock(mu_);
  auto it = last_sequence_.find(key);
  if (it != last_sequence_.end() && sequence <= it->second) {
    return true;
  }
  last_sequence_[key] = sequence;
  return false;
}

SnapshotProducer::Stats SnapshotProducer::GetStats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

}  // namespace cex::venues::app
