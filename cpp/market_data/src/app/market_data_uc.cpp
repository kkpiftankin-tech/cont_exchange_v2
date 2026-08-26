#include "app/market_data_uc.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "cex/common/log.hpp"
#include "infra/clickhouse/clickhouse_liquidity_curve_storage.hpp"
// F-05A (T-F05A-205): векторизация внешней ликвидности → marketdata.vectorized.
#include "app/curve_to_levels.hpp"
#include "app/ports/i_vectorized_publisher.hpp"
#include "transport/mappers/vectorized_liquidity.hpp"

namespace cex::market_data::app {

namespace {
// Нормализация символа: "BTC/USDT" → "BTCUSDT". FlowOrder/venues используют
// формат со слэшем, а REST API / кэш снимков — без. Ключи FlowOrderBook и
// last_snapshot_ должны совпадать, иначе BBO/depth не находятся.
std::string NormalizeAsset(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '/'), s.end());
  return s;
}
}  // namespace

MarketDataUseCases::MarketDataUseCases(
    IBatchOutputsStorage* batch_storage, IExecutionVenueStorage* execution_storage,
    domain::IOrderBookStorage* ob_storage,
    domain::IOrderBookPublisher* publisher, domain::ILiquidityCurveStorage* memory_curve_storage,
    infra::clickhouse::ClickHouseLiquidityCurveStorage* ch_curve_storage,
    domain::ISnapshotStorage* snapshot_storage,
    domain::IEffectiveSpreadStorage* spread_storage,
    domain::ISnapshotPublisher* snapshot_publisher,
    domain::IRiskAlertPublisher* risk_publisher,
    infra::MarketDataStreamHub* stream_hub,
    infra::PgMarketDataConfig* pg_config,
    IVectorizedPublisher* vectorized_publisher,
    MarketDataConfig md_config)
    : batch_storage_(batch_storage),
      execution_storage_(execution_storage),
      memory_curve_storage_(memory_curve_storage),
      ch_curve_storage_(ch_curve_storage),
      snapshot_storage_(snapshot_storage),
      spread_storage_(spread_storage),
      snapshot_publisher_(snapshot_publisher),
      risk_publisher_(risk_publisher),
      vectorized_publisher_(vectorized_publisher),
      stream_hub_(stream_hub),
      pg_config_(pg_config),
      md_config_(md_config),
      update_uc_(ob_storage, publisher) {}

std::string MarketDataUseCases::key(const std::string& venue, const std::string& symbol) {
  return venue + "|" + symbol;
}

std::string MarketDataUseCases::curve_key(const std::string& venue, const std::string& symbol,
                                          fob::venue::v1::ExecutionSide side) {
  std::string side_str = (side == fob::venue::v1::EXECUTION_SIDE_BUY) ? "BUY" : "SELL";
  return venue + "|" + symbol + "|" + side_str;
}

void MarketDataUseCases::OnMarketDataRaw(const fob::marketdata::v1::MarketDataRaw& evt) {
  if (!evt.has_ticker()) return;
  std::lock_guard<std::mutex> lg(mu_);
  const auto& t = evt.ticker();
  last_ticker_[key(t.venue(), t.instrument().symbol())] = t;
  update_uc_.OnMarketDataRaw(evt);
}

void MarketDataUseCases::OnBatchResult(const fob::matching::v1::BatchResult& batch) {
  {
    std::lock_guard<std::mutex> lg(mu_);
    ++batches_processed_;
    fills_processed_ += static_cast<uint64_t>(batch.fills_size());
    last_batch_id_ = batch.batch_id();
  }

  // F-05: СНАЧАЛА считаем и кэшируем MarketDataSnapshot (быстро, in-memory),
  // и только потом — тяжёлая ClickHouse-персистенция батча. Иначе блокирующие
  // CH-записи задерживали появление internal-снапшота, и его всегда перетирали
  // внешние venue-снапшоты.
  for (const auto& [raw_asset, _] : batch.clear_prices()) {
    // Нормализуем символ: "BTC/USDT" → "BTCUSDT" (matching и REST API используют разные форматы)
    const std::string asset = NormalizeAsset(raw_asset);

    // F5-3: bestBid/bestAsk из FOB FlowOrder aggregate curves (ключ — нормализованный)
    auto fob_bid = flow_order_book_.BestBid(asset);
    auto fob_ask = flow_order_book_.BestAsk(asset);

    // Передаём raw_asset для поиска в batch.clear_prices, но нормализованный asset для кэша
    auto snap = ComputeMarketData::FromBatchResult(
        batch, raw_asset, std::nullopt, md_config_, fob_bid, fob_ask);
    if (snap.has_value()) {
      // Переопределяем asset в снимке на нормализованный
      snap->asset = asset;
      // F5-6: глубина рынка из активных FlowOrder, агрегированная по уровням
      snap->bid_depth = flow_order_book_.DepthLevels(asset, true, md_config_.depth_levels);
      snap->ask_depth = flow_order_book_.DepthLevels(asset, false, md_config_.depth_levels);
      ProcessSnapshot(std::move(*snap));
    }
  }

  update_uc_.OnBatchResult(batch);

  if (batch_storage_) {
    const bool batch_ok = batch_storage_->SaveBatchResult(batch);
    const bool fills_ok = batch_storage_->SaveFills(batch);
    if (!batch_ok || !fills_ok) {
      cex::common::log_json("ERROR", "Failed to persist batch output",
                            {{"batch_id", batch.batch_id()},
                             {"batch_saved", batch_ok ? "true" : "false"},
                             {"fills_saved", fills_ok ? "true" : "false"}});
    }
  }

  cex::common::log_json("INFO", "MarketData processed batch.outputs",
                        {{"batch_id", batch.batch_id()},
                         {"clear_prices", std::to_string(batch.clear_prices_size())},
                         {"fills", std::to_string(batch.fills_size())}});
}

void MarketDataUseCases::OnOrderCreate(const fob::orders::v1::FlowOrder& order) {
  if (order.order_id().empty() || order.instrument().symbol().empty()) return;

  domain::FlowOrderBook::OrderEntry entry{
      .price_low  = common::Decimal::from_proto(order.price_low()),
      .price_high = common::Decimal::from_proto(order.price_high()),
      .max_speed  = common::Decimal::from_proto(order.max_speed()),
      .is_buy     = (order.side() == fob::common::v1::SIDE_BUY),
  };
  // Ключ нормализуем ("BTC/USDT" → "BTCUSDT"), чтобы BBO/depth-lookup по
  // нормализованному asset из OnBatchResult находил эти ордера.
  flow_order_book_.AddOrder(order.order_id(),
                            NormalizeAsset(order.instrument().symbol()), entry);
}

void MarketDataUseCases::OnOrderCancel(const std::string& order_id,
                                        const std::string& asset) {
  if (asset.empty()) {
    flow_order_book_.RemoveOrderById(order_id);
  } else {
    flow_order_book_.RemoveOrder(order_id, NormalizeAsset(asset));
  }
}

void MarketDataUseCases::OnFillEvent(const fob::matching::v1::FlowFill& fill,
                                     const std::string& fill_id,
                                     const std::string& batch_id,
                                     domain::Timestamp ts) {
  const std::string& asset = fill.instrument().symbol();
  auto mid_opt = GetCurrentMid(asset);
  if (!mid_opt.has_value()) {
    cex::common::log_json("WARN", "OnFillEvent: no mid for asset, skipping effective spread",
                          {{"asset", asset}, {"fill_id", fill_id}});
    return;
  }

  auto record = ComputeEffectiveSpread::FromFillEvent(fill, fill_id, batch_id, *mid_opt, ts);
  if (!record.has_value()) return;

  if (spread_storage_) {
    spread_storage_->SaveRecord(*record);
  }

  cex::common::log_json("INFO", "Computed effective spread",
                        {{"asset", asset},
                         {"fill_id", fill_id},
                         {"eff_spread_bps", record->effective_spread_bps.to_string()}});
}

void MarketDataUseCases::OnExecutionGroup(const fob::matching::v1::ExecutionGroup& eg) {
  // F-09 observability: ingest grouped combo execution into ClickHouse
  // (grouped_execution_events + grouped_leg_fills). Reuses the same analytics
  // storage as batch.outputs — idempotent (ReplacingMergeTree on event_time_ms).
  if (batch_storage_ == nullptr) {
    cex::common::log_json("WARN", "Batch output storage is not configured (execution.groups)",
                          {{"execution_group_id", eg.execution_group_id()}});
    return;
  }
  const bool ok = batch_storage_->SaveExecutionGroup(eg);
  cex::common::log_json(ok ? "INFO" : "ERROR", "MarketData processed execution.groups",
                        {{"execution_group_id", eg.execution_group_id()},
                         {"parent_order_id", eg.parent_order_id()},
                         {"legs", std::to_string(eg.leg_results_size())},
                         {"saved", ok ? "true" : "false"}});
}

void MarketDataUseCases::OnExecutionReport(
    const fob::execution::v1::ExecutionReport& report) {
  if (execution_storage_ == nullptr) {
    cex::common::log_json("WARN", "Execution report storage is not configured",
                          {{"intent_id", report.intent_id()},
                           {"report_id", report.report_id()}});
    return;
  }
  const bool ok = execution_storage_->SaveExecutionReport(report);
  cex::common::log_json(ok ? "INFO" : "ERROR", "MarketData processed execution.venue",
                        {{"intent_id", report.intent_id()},
                         {"report_id", report.report_id()},
                         {"venue", report.venue()},
                         {"symbol", report.instrument().symbol()},
                         {"stored", ok ? "true" : "false"}});
}

void MarketDataUseCases::OnLiquidityCurve(const fob::venue::v1::VenueLiquidityCurve& curve) {
  // Save to in-memory storage for fast access
  if (memory_curve_storage_ != nullptr) {
    memory_curve_storage_->Store(curve);
  } else {
    cex::common::log_json("WARN", "Memory curve storage is not configured",
                          {{"venue", curve.venue_id()}, {"symbol", curve.instrument().symbol()}});
  }

  // Save to ClickHouse for historical analytics
  if (ch_curve_storage_ != nullptr) {
    ch_curve_storage_->Save(curve);
  } else {
    cex::common::log_json("WARN", "ClickHouse curve storage is not configured",
                          {{"venue", curve.venue_id()}, {"symbol", curve.instrument().symbol()}});
  }

  cex::common::log_json("INFO", "Stored liquidity curve",
                        {{"service", "market_data"},
                         {"component", "market_data_service"},
                         {"participant", "Market Data Service"},
                         {"stage", "store_liquidity_curve"},
                         {"topic", "venue.liquidity.fob"},
                         {"venue", curve.venue_id()},
                         {"symbol", curve.instrument().symbol()},
                         {"curve_id", curve.curve_id()},
                         {"snapshot_id", curve.snapshot_id()},
                         {"confidence", std::to_string(curve.confidence())},
                         {"level", curve.level()},
                         {"has_bid", curve.has_bid_curve() ? "true" : "false"},
                         {"has_ask", curve.has_ask_curve() ? "true" : "false"},
                         {"memory_store_enabled",
                          memory_curve_storage_ != nullptr ? "true" : "false"},
                         {"clickhouse_store_enabled",
                          ch_curve_storage_ != nullptr ? "true" : "false"},
                         {"source_file", "cpp/market_data/src/app/market_data_uc.cpp"}});

  // F-05A (T-F05A-205): векторизация кривой → сегменты W → marketdata.vectorized.
  if (vectorized_publisher_ != nullptr) {
    auto levels = LevelsFromCurve(curve, vectorize_cfg_.decimal_scale);
    domain::VectorizeResult vr = domain::Vectorize(levels, vectorize_cfg_);
    if (!vr.segments.empty()) {
      const std::string batch_id =
          !curve.snapshot_id().empty()
              ? curve.snapshot_id()
              : (!curve.curve_id().empty()
                     ? curve.curve_id()
                     : key(curve.venue_id(), curve.instrument().symbol()));
      const long long ts_ms = curve.timestamp().seconds() * 1000 +
                              curve.timestamp().nanos() / 1000000;
      auto snap = transport::ToVectorizedSnapshot(vr, batch_id, ts_ms,
                                                  vectorize_cfg_.decimal_scale);
      vectorized_publisher_->Publish(snap);
      cex::common::log_json("INFO", "Published vectorized liquidity",
                            {{"service", "market_data"},
                             {"stage", "vectorize_publish"},
                             {"topic", "marketdata.vectorized"},
                             {"venue", curve.venue_id()},
                             {"symbol", curve.instrument().symbol()},
                             {"batch_id", batch_id},
                             {"num_segments", std::to_string(vr.segments.size())},
                             {"num_assets", std::to_string(vr.basis.num_assets)}});
    }
  }
}

void MarketDataUseCases::OnVenueSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) {
  update_uc_.OnVenueSnapshot(snapshot);

  // F-05: при поступлении внешних котировок обновляем composite-метрики
  // Нормализуем символ: "BTC/USDT" → "BTCUSDT" (venues и REST API используют разные форматы)
  std::string asset = snapshot.instrument().symbol();
  asset.erase(std::remove(asset.begin(), asset.end(), '/'), asset.end());
  auto snap = ComputeMarketData::FromVenueSnapshot(snapshot, asset, md_config_);
  if (!snap.has_value()) return;

  // Записываем в кэш только если нет свежего внутреннего снимка.
  // CEX и DEX имеют равный приоритет — кэш обновляет последний пришедший
  // внешний снимок (если internal отсутствует или устарел).
  {
    std::lock_guard<std::mutex> lg(mu_);
    const auto it = last_snapshot_.find(asset);
    const bool has_fresh_internal = (it != last_snapshot_.end() &&
                                     it->second.source == domain::DataSource::Internal &&
                                     !it->second.stale);
    if (has_fresh_internal) return;
  }
  // ProcessSnapshot берёт мьютекс сам — вызываем без блокировки
  cex::common::log_json("INFO", "F-05: cached external snapshot",
                        {{"asset", asset}, {"source", "cex"}});
  ProcessSnapshot(std::move(*snap));
}

void MarketDataUseCases::ProcessSnapshot(domain::MarketDataSnapshot snap) {
  // Kill-switch: проверяем isActive из marketdata_config
  if (pg_config_ && !pg_config_->IsActive(snap.asset)) {
    cex::common::log_json("INFO", "MarketData kill-switch active, skipping publish",
                          {{"asset", snap.asset}});
    // Обновляем кэш для REST-доступа, но не публикуем
    std::lock_guard<std::mutex> lg(mu_);
    last_snapshot_[snap.asset] = snap;
    // Уведомляем WS-клиентов что инструмент на паузе
    if (stream_hub_) {
      domain::MarketDataSnapshot paused = snap;
      paused.stale = true;
      stream_hub_->Broadcast(paused);
    }
    return;
  }

  // 1. Сохраняем в in-memory кэш
  {
    std::lock_guard<std::mutex> lg(mu_);
    last_snapshot_[snap.asset] = snap;
  }

  // 2. volume24h + персист в ClickHouse — ТРОТТЛИНГ на asset (не чаще раза в 15с).
  //    На каждый снапшот (особенно высокочастотные venue.snapshots) синхронный
  //    CH-запрос перегружал ClickHouse и блокировал консьюмер. Между обновлениями
  //    берём volume из кэша; in-memory last_snapshot_ уже обновлён выше.
  if (snapshot_storage_) {
    const auto now = std::chrono::system_clock::now();
    bool persist = false;
    common::Decimal vol = common::Decimal::zero();
    {
      std::lock_guard<std::mutex> lg(mu_);
      const auto pit = last_ch_persist_.find(snap.asset);
      persist = (pit == last_ch_persist_.end() ||
                 (now - pit->second) >= std::chrono::seconds(15));
      const auto vit = volume24h_cache_.find(snap.asset);
      if (vit != volume24h_cache_.end()) vol = vit->second;
    }
    if (persist) {
      const uint32_t window =
          md_config_.volume_window_sec > 0 ? md_config_.volume_window_sec : 86400;
      const auto fresh = snapshot_storage_->GetVolume24h(snap.asset, window);  // CH read
      if (common::Decimal::cmp(fresh, common::Decimal::zero()) > 0) vol = fresh;
      if (common::Decimal::cmp(vol, common::Decimal::zero()) > 0) snap.volume_24h = vol;
      snapshot_storage_->SaveSnapshot(snap);  // CH write
      std::lock_guard<std::mutex> lg(mu_);
      volume24h_cache_[snap.asset] = vol;
      last_ch_persist_[snap.asset] = now;
      last_snapshot_[snap.asset] = snap;
    } else if (common::Decimal::cmp(vol, common::Decimal::zero()) > 0) {
      snap.volume_24h = vol;
      std::lock_guard<std::mutex> lg(mu_);
      last_snapshot_[snap.asset] = snap;
    }
  }

  // 3. Публикуем в Kafka marketdata.snapshots
  if (snapshot_publisher_) {
    snapshot_publisher_->PublishSnapshot(snap);
  }

  // 4. Транслируем активным gRPC-стримам (→ WebSocket клиентам)
  if (stream_hub_) {
    stream_hub_->Broadcast(snap);
  }

  // 4. Проверяем на аномальный спред — генерируем risk alert
  // Threshold читается из PG динамически (кэш 30с), fallback на startup config
  if (risk_publisher_) {
    double threshold = md_config_.spread_alert_threshold_bps;
    if (pg_config_) {
      threshold = pg_config_->GetConfig(snap.asset).spread_alert_threshold_bps;
    }
    const double spread_bps = static_cast<double>(snap.spread_bps);
    if (spread_bps > threshold) {
      risk_publisher_->PublishSpreadAlert(snap.asset, snap.spread_bps,
                                          {static_cast<int64_t>(threshold * 1e8), 8});
    }
  }
}

std::optional<domain::MarketDataSnapshot> MarketDataUseCases::GetMarketDataSnapshot(
    const std::string& asset) const {
  std::lock_guard<std::mutex> lg(mu_);
  const auto it = last_snapshot_.find(asset);
  if (it == last_snapshot_.end()) return std::nullopt;
  return it->second;
}

std::vector<domain::MarketDataSnapshot> MarketDataUseCases::GetReferencePrices(
    const std::vector<std::string>& assets) const {
  std::vector<domain::MarketDataSnapshot> result;
  result.reserve(assets.size());
  std::lock_guard<std::mutex> lg(mu_);
  for (const auto& asset : assets) {
    const auto it = last_snapshot_.find(asset);
    if (it != last_snapshot_.end()) {
      result.push_back(it->second);
    }
  }
  return result;
}

void MarketDataUseCases::StartStaleSweeper() {
  sweeper_running_.store(true);
  sweeper_thread_ = std::thread([this] {
    const auto threshold =
        std::chrono::seconds(md_config_.stale_threshold_sec > 0
                                 ? md_config_.stale_threshold_sec
                                 : 30);
    while (sweeper_running_.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      const auto now = std::chrono::system_clock::now();
      std::lock_guard<std::mutex> lg(mu_);
      for (auto& [asset, snap] : last_snapshot_) {
        const bool is_old = (now - snap.timestamp) > threshold;
        if (is_old && !snap.stale) {
          snap.stale = true;
          cex::common::log_json("WARN", "MarketData snapshot marked stale",
                                {{"asset", asset}});
          if (stream_hub_) stream_hub_->Broadcast(snap);
        }
      }
    }
  });
}

void MarketDataUseCases::StopStaleSweeper() {
  sweeper_running_.store(false);
  if (sweeper_thread_.joinable()) sweeper_thread_.join();
}

std::optional<common::Decimal> MarketDataUseCases::GetCurrentMid(
    const std::string& asset) const {
  std::lock_guard<std::mutex> lg(mu_);
  const auto it = last_snapshot_.find(asset);
  if (it == last_snapshot_.end()) return std::nullopt;
  return it->second.mid;
}

std::optional<fob::marketdata::v1::Ticker> MarketDataUseCases::GetLastTicker(
    const std::string& venue, const std::string& symbol) const {
  std::lock_guard<std::mutex> lg(mu_);
  auto it = last_ticker_.find(key(venue, symbol));
  if (it == last_ticker_.end()) return std::nullopt;
  return it->second;
}

std::optional<fob::venue::v1::SideLiquidityCurve> MarketDataUseCases::GetLiquidityCurve(
    const std::string& venue, const std::string& symbol, fob::venue::v1::ExecutionSide side) const {
  if (memory_curve_storage_ == nullptr) {
    return std::nullopt;
  }
  return memory_curve_storage_->GetCurve(venue, symbol, side);
}

MarketDataUseCases::BatchOutputsStats MarketDataUseCases::GetBatchOutputsStats() const {
  std::lock_guard<std::mutex> lg(mu_);
  return BatchOutputsStats{
      .batches_processed = batches_processed_,
      .fills_processed = fills_processed_,
      .last_batch_id = last_batch_id_,
  };
}

}  // namespace cex::market_data::app
