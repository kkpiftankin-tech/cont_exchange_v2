#include "infra/kafka_snapshots_producer.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "fob/marketdata/v1/marketdata_service.pb.h"
#include "fob/risk/v1/risk.pb.h"

namespace cex::market_data::infra {

// ---------------------------------------------------------------------------
// KafkaSnapshotsProducer
// ---------------------------------------------------------------------------

KafkaSnapshotsProducer::KafkaSnapshotsProducer(const std::string& brokers,
                                                 const std::string& topic)
    : topic_(topic),
      producer_(cex::common::KafkaConfig{.brokers = brokers,
                                          .client_id = "market_data_snapshots"}) {}

void KafkaSnapshotsProducer::PublishSnapshot(const domain::MarketDataSnapshot& snap) {
  // Маппим domain → proto MarketDataSnapshot
  fob::marketdata::v1::MarketDataSnapshot proto;
  proto.set_snapshot_id(snap.snapshot_id);
  proto.set_asset(snap.asset);
  proto.mutable_mid()->CopyFrom(snap.mid.to_proto());
  proto.mutable_best_bid()->CopyFrom(snap.best_bid.to_proto());
  proto.mutable_best_ask()->CopyFrom(snap.best_ask.to_proto());
  proto.mutable_spread()->CopyFrom(snap.spread.to_proto());
  proto.mutable_spread_bps()->CopyFrom(snap.spread_bps.to_proto());
  proto.mutable_volume_24h()->CopyFrom(snap.volume_24h.to_proto());
  proto.mutable_clear_price()->CopyFrom(snap.clear_price.to_proto());
  proto.mutable_executed_rate()->CopyFrom(snap.executed_rate.to_proto());
  proto.set_batch_id(snap.batch_id);

  switch (snap.source) {
    case domain::DataSource::Cex:       proto.set_source("cex");       break;
    case domain::DataSource::Dex:       proto.set_source("dex");       break;
    case domain::DataSource::Composite: proto.set_source("composite"); break;
    default:                            proto.set_source("internal");  break;
  }
  proto.set_stale(snap.stale);

  for (const auto& dl : snap.bid_depth) {
    auto* level = proto.add_bid_depth();
    level->mutable_price()->CopyFrom(dl.price.to_proto());
    level->mutable_qty()->CopyFrom(dl.qty.to_proto());
  }
  for (const auto& dl : snap.ask_depth) {
    auto* level = proto.add_ask_depth();
    level->mutable_price()->CopyFrom(dl.price.to_proto());
    level->mutable_qty()->CopyFrom(dl.qty.to_proto());
  }

  std::string bytes;
  if (!proto.SerializeToString(&bytes)) {
    cex::common::log_json("ERROR", "Failed to serialize MarketDataSnapshot",
                          {{"asset", snap.asset}});
    return;
  }

  // Partition key = asset — все снимки по одному инструменту идут в один partition
  const bool ok = producer_.produce(topic_, snap.asset, bytes);
  if (!ok) {
    cex::common::log_json("ERROR", "KafkaSnapshotsProducer: produce failed",
                          {{"topic", topic_}, {"asset", snap.asset}});
  }
}

// ---------------------------------------------------------------------------
// KafkaRiskAlertPublisher
// ---------------------------------------------------------------------------

KafkaRiskAlertPublisher::KafkaRiskAlertPublisher(const std::string& brokers,
                                                   const std::string& topic)
    : topic_(topic),
      producer_(cex::common::KafkaConfig{.brokers = brokers,
                                          .client_id = "market_data_risk_alerts"}) {}

void KafkaRiskAlertPublisher::PublishSpreadAlert(const std::string& asset,
                                                  const common::Decimal& spread_bps,
                                                  const common::Decimal& threshold_bps) {
  fob::risk::v1::RiskAlert alert;
  alert.set_alert_type("WIDE_SPREAD");
  alert.set_instrument_symbol(asset);
  (*alert.mutable_details())["spread_bps"]    = spread_bps.to_string();
  (*alert.mutable_details())["threshold_bps"] = threshold_bps.to_string();

  std::string bytes;
  if (!alert.SerializeToString(&bytes)) {
    cex::common::log_json("ERROR", "Failed to serialize RiskAlert", {{"asset", asset}});
    return;
  }

  producer_.produce(topic_, asset, bytes);
  cex::common::log_json("WARN", "Spread alert published",
                        {{"asset", asset},
                         {"spread_bps", spread_bps.to_string()},
                         {"threshold_bps", threshold_bps.to_string()}});
}

}  // namespace cex::market_data::infra
