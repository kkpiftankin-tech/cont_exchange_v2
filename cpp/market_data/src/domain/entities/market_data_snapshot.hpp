#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/entities/fill_event.hpp"

namespace cex::market_data::domain {

enum class DataSource {
  Internal,
  Cex,
  Dex,
  Composite,
};

struct DepthLevel {
  common::Decimal price;
  common::Decimal qty;
};

// Полный снимок рыночных данных по инструменту.
// Результат ComputeMarketData после BatchResult или updateComposite после VenueSnapshot.
struct MarketDataSnapshot {
  std::string     snapshot_id;
  std::string     asset;

  common::Decimal mid;
  common::Decimal best_bid;
  common::Decimal best_ask;
  common::Decimal spread;
  common::Decimal spread_bps;

  common::Decimal volume_24h;
  common::Decimal volume_quote_24h;

  std::vector<DepthLevel> bid_depth;
  std::vector<DepthLevel> ask_depth;

  common::Decimal clear_price;
  common::Decimal executed_rate;
  std::string     batch_id;

  DataSource source{DataSource::Internal};
  bool       stale{false};

  Timestamp timestamp;
};

// Запись effective spread, рассчитывается post-trade для каждого FillEvent.
struct EffectiveSpreadRecord {
  std::string     fill_id;
  std::string     asset;
  common::Decimal exec_price;
  common::Decimal mid_at_exec;
  common::Decimal effective_spread;
  common::Decimal effective_spread_bps;
  std::string     batch_id;
  Timestamp       timestamp;
};

}  // namespace cex::market_data::domain
