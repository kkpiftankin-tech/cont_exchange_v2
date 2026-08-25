#include "app/compute_market_data.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

#include "cex/common/log.hpp"
#include "cex/common/uuid.hpp"

namespace cex::market_data::app {

namespace {

// double → Decimal с фиксированным scale=8 (достаточно для bps, metrics).
common::Decimal FromDouble(double v, int32_t scale = 8) {
  int64_t units = static_cast<int64_t>(std::round(v * std::pow(10.0, scale)));
  return {units, scale};
}

// Классификация внешней площадки по venue_id: DEX/AMM → Dex, иначе Cex.
domain::DataSource ClassifyVenueSource(const std::string& venue_id) {
  std::string v = venue_id;
  std::transform(v.begin(), v.end(), v.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  static const char* kDex[] = {"uniswap", "sushi", "pancake", "curve",
                               "balancer", "dex", "amm"};
  for (const char* d : kDex) {
    if (v.find(d) != std::string::npos) return domain::DataSource::Dex;
  }
  return domain::DataSource::Cex;
}

common::Decimal Divide(const common::Decimal& num, const common::Decimal& den) {
  // Для метрик (spreadBps) используем double-арифметику.
  double n = static_cast<double>(num);
  double d = static_cast<double>(den);
  if (std::abs(d) < 1e-18) return common::Decimal::zero();
  return FromDouble(n / d);
}

}  // namespace

// ---------------------------------------------------------------------------
// ComputeMarketData
// ---------------------------------------------------------------------------

std::optional<domain::MarketDataSnapshot> ComputeMarketData::FromBatchResult(
    const fob::matching::v1::BatchResult& batch,
    const std::string& asset,
    const std::optional<domain::OrderBook>& current_book,
    const MarketDataConfig& cfg,
    std::optional<common::Decimal> fob_best_bid,
    std::optional<common::Decimal> fob_best_ask) {
  const auto clear_price = ExtractClearPrice(batch, asset);
  if (common::Decimal::cmp(clear_price, common::Decimal::zero()) <= 0) {
    cex::common::log_json("WARN", "ComputeMarketData: no clear_price for asset",
                          {{"asset", asset}, {"batch_id", batch.batch_id()}});
    return std::nullopt;
  }

  domain::MarketDataSnapshot snap;
  snap.snapshot_id = cex::common::uuid_v4();
  snap.asset       = asset;
  snap.batch_id    = batch.batch_id();
  snap.source      = domain::DataSource::Internal;
  snap.stale       = false;
  snap.timestamp   = std::chrono::system_clock::now();
  snap.clear_price = clear_price;

  // executed_rate: первый executedRate по любому ордеру этого asset
  for (const auto& [order_id, rate] : batch.executed_rates()) {
    snap.executed_rate = common::Decimal::from_proto(rate);
    break;
  }

  // F5-3: приоритет — FOB aggregate curves (FlowOrder bestBid/bestAsk)
  if (fob_best_bid.has_value() && fob_best_ask.has_value()) {
    snap.best_bid = *fob_best_bid;
    snap.best_ask = *fob_best_ask;
    // Depth из venue OrderBook если доступен
    if (current_book.has_value()) {
      snap.bid_depth = ExtractDepth(*current_book, true, cfg.depth_levels);
      snap.ask_depth = ExtractDepth(*current_book, false, cfg.depth_levels);
    }
  } else if (current_book.has_value() && !current_book->bid_depth.empty() &&
             !current_book->ask_depth.empty()) {
    // Fallback: venue OrderBook (MVP)
    const domain::BBO bbo = current_book->GetBBO();
    snap.best_bid = FromDouble(bbo.bid_price);
    snap.best_ask = FromDouble(bbo.ask_price);
    snap.bid_depth = ExtractDepth(*current_book, true, cfg.depth_levels);
    snap.ask_depth = ExtractDepth(*current_book, false, cfg.depth_levels);
  } else {
    // Нет данных — clear_price как единственная точка
    snap.best_bid = clear_price;
    snap.best_ask = clear_price;
  }

  // mid = (bestBid + bestAsk) / 2
  snap.mid = Divide(common::Decimal::add(snap.best_bid, snap.best_ask),
                    FromDouble(2.0, 0));

  // spread и spreadBps
  snap.spread = common::Decimal::sub(snap.best_ask, snap.best_bid);
  if (common::Decimal::cmp(snap.mid, common::Decimal::zero()) > 0) {
    double spread_d = static_cast<double>(snap.spread);
    double mid_d    = static_cast<double>(snap.mid);
    snap.spread_bps = FromDouble(spread_d / mid_d * 10000.0);
  }

  // volume24h: сумма execQty по данному asset в текущем batch
  snap.volume_24h = ComputeVolume24hFromBatch(batch, asset);

  return snap;
}

std::optional<domain::MarketDataSnapshot> ComputeMarketData::FromVenueSnapshot(
    const fob::venue::v1::VenueSnapshot& snapshot,
    const std::string& asset,
    const MarketDataConfig& cfg) {
  // VenueSnapshot хранит best_bid/best_ask напрямую (нет вложенного bbo-поля)
  if (!snapshot.has_best_bid() || !snapshot.has_best_ask()) return std::nullopt;

  domain::MarketDataSnapshot snap;
  snap.snapshot_id = cex::common::uuid_v4();
  snap.asset       = asset;
  // F5-12: cex|dex по типу площадки (binance→cex, uniswap_v3→dex)
  snap.source      = ClassifyVenueSource(snapshot.venue_id());
  snap.stale       = false;
  snap.timestamp   = std::chrono::system_clock::now();

  // Нормализуем к scale=8 чтобы best_bid/best_ask/mid использовали единый scale
  snap.best_bid = FromDouble(static_cast<double>(common::Decimal::from_proto(snapshot.best_bid())));
  snap.best_ask = FromDouble(static_cast<double>(common::Decimal::from_proto(snapshot.best_ask())));

  snap.mid    = Divide(common::Decimal::add(snap.best_bid, snap.best_ask),
                       FromDouble(2.0, 0));
  snap.spread = common::Decimal::sub(snap.best_ask, snap.best_bid);

  if (common::Decimal::cmp(snap.mid, common::Decimal::zero()) > 0) {
    double spread_d = static_cast<double>(snap.spread);
    double mid_d    = static_cast<double>(snap.mid);
    snap.spread_bps = FromDouble(spread_d / mid_d * 10000.0);
  }

  // F5-6: глубина рынка из полного стакана внешней площадки (до depth_levels)
  const int n_bid = std::min(snapshot.bid_prices_size(), snapshot.bid_quantities_size());
  for (int i = 0; i < n_bid && snap.bid_depth.size() < cfg.depth_levels; ++i) {
    snap.bid_depth.push_back(
        {FromDouble(static_cast<double>(common::Decimal::from_proto(snapshot.bid_prices(i)))),
         FromDouble(static_cast<double>(common::Decimal::from_proto(snapshot.bid_quantities(i))))});
  }
  const int n_ask = std::min(snapshot.ask_prices_size(), snapshot.ask_quantities_size());
  for (int i = 0; i < n_ask && snap.ask_depth.size() < cfg.depth_levels; ++i) {
    snap.ask_depth.push_back(
        {FromDouble(static_cast<double>(common::Decimal::from_proto(snapshot.ask_prices(i)))),
         FromDouble(static_cast<double>(common::Decimal::from_proto(snapshot.ask_quantities(i))))});
  }

  return snap;
}

common::Decimal ComputeMarketData::ExtractClearPrice(
    const fob::matching::v1::BatchResult& batch,
    const std::string& asset) {
  const auto& prices = batch.clear_prices();
  const auto  it     = prices.find(asset);
  if (it == prices.end()) return common::Decimal::zero();
  return common::Decimal::from_proto(it->second);
}

common::Decimal ComputeMarketData::ComputeVolume24hFromBatch(
    const fob::matching::v1::BatchResult& batch,
    const std::string& asset) {
  // В MVP суммируем execQty только из текущего batch.
  // Полное 24h-окно читается из ClickHouse в SnapshotStorage.
  common::Decimal total = common::Decimal::zero();
  for (const auto& fill : batch.fills()) {
    if (fill.instrument().symbol() == asset) {
      total = common::Decimal::add(total,
                                   common::Decimal::from_proto(fill.executed_qty()));
    }
  }
  return total;
}

std::vector<domain::DepthLevel> ComputeMarketData::ExtractDepth(
    const domain::OrderBook& book,
    bool bid_side,
    uint32_t levels) {
  std::vector<domain::DepthLevel> result;
  result.reserve(levels);
  uint32_t count = 0;

  if (bid_side) {
    for (const auto& pl : book.bid_depth) {
      if (count++ >= levels) break;
      result.push_back({pl.price, pl.quantity});
    }
  } else {
    for (const auto& pl : book.ask_depth) {
      if (count++ >= levels) break;
      result.push_back({pl.price, pl.quantity});
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// ComputeEffectiveSpread
// ---------------------------------------------------------------------------

std::optional<domain::EffectiveSpreadRecord> ComputeEffectiveSpread::FromFillEvent(
    const fob::matching::v1::FlowFill& fill,
    const std::string& fill_id,
    const std::string& batch_id,
    const common::Decimal& current_mid,
    domain::Timestamp ts) {
  if (common::Decimal::cmp(current_mid, common::Decimal::zero()) <= 0) {
    return std::nullopt;
  }

  const auto exec_price = common::Decimal::from_proto(fill.price());

  // effectiveSpread = 2 * |execPrice - mid|
  auto diff = common::Decimal::sub(exec_price, current_mid);
  // abs(diff) — используем double для abs, затем конвертируем обратно
  double diff_d = std::abs(static_cast<double>(diff));
  auto abs_diff = FromDouble(diff_d);
  auto eff_spread = common::Decimal::mul(FromDouble(2.0, 0), abs_diff);

  // effectiveSpreadBps = effectiveSpread / mid * 10000
  double mid_d     = static_cast<double>(current_mid);
  double eff_bps   = (mid_d > 1e-18) ? (diff_d * 2.0 / mid_d * 10000.0) : 0.0;

  domain::EffectiveSpreadRecord record;
  record.fill_id             = fill_id;
  record.asset               = fill.instrument().symbol();
  record.exec_price          = exec_price;
  record.mid_at_exec         = current_mid;
  record.effective_spread    = eff_spread;
  record.effective_spread_bps = FromDouble(eff_bps);
  record.batch_id            = batch_id;
  record.timestamp           = ts;

  return record;
}

}  // namespace cex::market_data::app
