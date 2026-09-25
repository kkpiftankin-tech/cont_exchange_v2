#include "app/market_data_uc.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include <pqxx/pqxx>  // ADR-050/052: runtime-конфиг окна клиринга из Postgres

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"
#include "infra/clickhouse/clickhouse_liquidity_curve_storage.hpp"
// F-05A (T-F05A-205): векторизация внешней ликвидности → marketdata.vectorized.
#include <cstdlib>  // std::getenv/atof — F-05A CE θ/params
#include <map>
#include <set>

#include "app/curve_to_levels.hpp"
#include "app/ports/i_ce_clearing_publisher.hpp"  // F-05A CE (вариант A)
#include "app/ports/i_vectorized_publisher.hpp"
#include "domain/agent_builder.hpp"  // F-05A CE §A1
#include "app/ports/i_vector_segment_storage.hpp"
#include "app/ports/i_vector_clearing_result_storage.hpp"
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

// F-05A ADR-050: масштаб Decimal на вес свежести f∈[0,1] (для q_max сегмента).
cex::common::Decimal ScaleDecimal(const cex::common::Decimal& d, double f) {
  cex::common::Decimal out = d;
  out.units = static_cast<std::int64_t>(
      std::llround(static_cast<double>(d.units) * f));
  return out;
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
    MarketDataConfig md_config,
    ICeClearingPublisher* ce_publisher)
    : batch_storage_(batch_storage),
      execution_storage_(execution_storage),
      memory_curve_storage_(memory_curve_storage),
      ch_curve_storage_(ch_curve_storage),
      snapshot_storage_(snapshot_storage),
      spread_storage_(spread_storage),
      snapshot_publisher_(snapshot_publisher),
      risk_publisher_(risk_publisher),
      vectorized_publisher_(vectorized_publisher),
      ce_publisher_(ce_publisher),
      stream_hub_(stream_hub),
      pg_config_(pg_config),
      md_config_(md_config),
      update_uc_(ob_storage, publisher) {
  // F-05A ADR-050: batch-window aggregation config (env, dev-defaults).
  vector_window_enabled_ = cex::common::Env::get_bool("F05A_BATCH_WINDOW_ENABLED", false);
  vector_linear_segments_ = cex::common::Env::get_bool("F05A_LINEAR_SEGMENTS_ENABLED", false);
  vector_two_sided_ = cex::common::Env::get_bool("F05A_TWO_SIDED_ENABLED", false);
  ce_agents_enabled_ = cex::common::Env::get_bool("CE_AGENTS_ENABLED", false);
  vector_window_ms_ = static_cast<std::int64_t>(
      cex::common::Env::get_int("F05A_BATCH_WINDOW_MS", 1000));
  vector_stale_ms_ = static_cast<std::int64_t>(
      cex::common::Env::get_int("F05A_STALE_LEVEL_MS", 2000));
  clearing_cfg_pg_conn_ = cex::common::Env::get_string(
      "POSTGRES_CONN",
      "host=postgres port=5432 dbname=exchange user=exchange password=exchange");
}

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

  // F-05A ADR-050: в оконном режиме кривая НЕ векторизуется поканально — кладётся
  // в буфер (новая вытесняет старую на ключ venue|pair); агрегированный клиринг
  // над общим asset-basis собирает таймер FlushVectorWindow.
  if (vector_window_enabled_) {
    const std::int64_t ts_ms = curve.timestamp().seconds() * 1000 +
                               curve.timestamp().nanos() / 1000000;
    std::lock_guard<std::mutex> lk(vector_window_mu_);
    vector_window_buffer_[key(curve.venue_id(), curve.instrument().symbol())] =
        BufferedCurve{curve, ts_ms};
    return;
  }

  // F-05A (T-F05A-205/206): векторизация кривой → сегменты W → publish + persist.
  if (vectorized_publisher_ != nullptr || vector_segment_storage_ != nullptr) {
    auto levels = vector_linear_segments_  // ADR-051: 1 линейный сегмент/сторону
                      ? LinearSegmentsFromCurve(curve, vectorize_cfg_.decimal_scale)
                      : LevelsFromCurve(curve, vectorize_cfg_.decimal_scale);
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
      if (vectorized_publisher_ != nullptr) {
        auto snap = transport::ToVectorizedSnapshot(vr, batch_id, ts_ms,
                                                    vectorize_cfg_.decimal_scale);
        vectorized_publisher_->Publish(snap);
      }
      if (vector_segment_storage_ != nullptr) {  // T-F05A-206: CH persist
        vector_segment_storage_->SaveSegments(batch_id, vr, ts_ms);
      }
      cex::common::log_json("INFO", "Vectorized liquidity",
                            {{"service", "market_data"},
                             {"stage", "vectorize"},
                             {"topic", "marketdata.vectorized"},
                             {"venue", curve.venue_id()},
                             {"symbol", curve.instrument().symbol()},
                             {"batch_id", batch_id},
                             {"num_segments", std::to_string(vr.segments.size())},
                             {"num_assets", std::to_string(vr.basis.num_assets)},
                             {"published",
                              vectorized_publisher_ != nullptr ? "true" : "false"},
                             {"persisted",
                              vector_segment_storage_ != nullptr ? "true" : "false"}});
    }
  }
}

// F-05A (T-F05A-305 persister): consume matching.vector_clearing → CH
// vector_clearing_results (аналитика). Денег не трогает.
void MarketDataUseCases::OnVectorClearingResult(
    const fob::marketdata::v1::VectorClearingResult& result) {
  if (vector_clearing_result_storage_ != nullptr) {
    vector_clearing_result_storage_->SaveResult(result);
  }
  cex::common::log_json("INFO", "Vector clearing result",
                        {{"service", "market_data"},
                         {"stage", "vector_clearing_persist"},
                         {"topic", "matching.vector_clearing"},
                         {"batch_id", result.batch_id()},
                         {"solver_status", std::to_string(result.solver_status())},
                         {"leg_count", std::to_string(result.x_size())},
                         {"persisted",
                          vector_clearing_result_storage_ != nullptr ? "true" : "false"}});
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

// ADR-050/052: runtime-конфиг окна из PG (таблица f05a_clearing_config). TTL 2с,
// чтобы UI-изменения применялись быстро, но без нагрузки на PG. Ошибки PG молча
// игнорируются (остаются текущие значения). Вызывается только из таймер-потока.
void MarketDataUseCases::RefreshClearingConfigFromPg() {
  const std::int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  if (now_ms - clearing_cfg_last_read_ms_ < 2000) return;  // TTL
  clearing_cfg_last_read_ms_ = now_ms;
  try {
    pqxx::connection c(clearing_cfg_pg_conn_);
    pqxx::work tx(c);
    const pqxx::row r = tx.exec1(
        "SELECT batch_window_ms, stale_level_ms, ce_taker_fee_bps"
        " FROM f05a_clearing_config WHERE id=1");
    const std::int64_t w = r[0].as<std::int64_t>();
    const std::int64_t s = r[1].as<std::int64_t>();
    if (w >= 100 && w <= 600000) vector_window_ms_ = w;   // [100мс, 10мин]
    if (s >= 100 && s <= 3600000) vector_stale_ms_ = s;   // [100мс, 60мин]
    // Комиссия тейкера, настраиваемая с фронта: <0 = из стакана venue; 0 = линейные кривые.
    const double fee = r[2].as<double>();
    if (fee <= 1000.0) ce_taker_fee_bps_ = fee;           // санити: не больше 100%
  } catch (const std::exception&) {
    // PG недоступен/нет строки — оставляем текущие значения.
  }
}

// F-05A ADR-050: таймер-поток окна. No-op, если флаг выключен.
void MarketDataUseCases::StartVectorWindow() {
  if (!vector_window_enabled_) return;
  vector_window_running_.store(true);
  vector_window_thread_ = std::thread([this] {
    while (vector_window_running_.load()) {
      RefreshClearingConfigFromPg();  // подхватываем runtime-настройки окна
      std::this_thread::sleep_for(std::chrono::milliseconds(vector_window_ms_));
      if (!vector_window_running_.load()) break;
      const std::int64_t now_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      FlushVectorWindow(now_ms);
    }
  });
  cex::common::log_json("INFO", "F-05A batch-window aggregator started",
                        {{"service", "market_data"},
                         {"window_ms", std::to_string(vector_window_ms_)},
                         {"stale_ms", std::to_string(vector_stale_ms_)}});
}

void MarketDataUseCases::StopVectorWindow() {
  vector_window_running_.store(false);
  if (vector_window_thread_.joinable()) vector_window_thread_.join();
}

// Собрать окно: свежие кривые всех venue×pair → общий asset-basis → один
// marketdata.vectorized. Вес свежести f убывает с возрастом и масштабирует q_max;
// за жёстким порогом уровень отбрасывается (ADR-050).
void MarketDataUseCases::FlushVectorWindow(std::int64_t window_close_ms) {
  std::vector<BufferedCurve> fresh;
  {
    std::lock_guard<std::mutex> lk(vector_window_mu_);
    fresh.reserve(vector_window_buffer_.size());
    for (const auto& [k, bc] : vector_window_buffer_) fresh.push_back(bc);
  }
  if (fresh.empty()) return;

  // Детерминированный порядок (ADR-050 §7, AC-F05A-010): unordered_map даёт
  // непредсказуемый порядок → seg_index/x «плавали» бы, ломая replay F-15.
  std::sort(fresh.begin(), fresh.end(), [](const BufferedCurve& a, const BufferedCurve& b) {
    if (a.curve.venue_id() != b.curve.venue_id())
      return a.curve.venue_id() < b.curve.venue_id();
    return a.curve.instrument().symbol() < b.curve.instrument().symbol();
  });

  std::vector<domain::ExternalOrderLevel> levels;   // per-level / linear путь
  std::vector<fob::venue::v1::VenueLiquidityCurve> fresh_curves;  // two-sided путь
  int stale_dropped = 0;
  for (const auto& bc : fresh) {
    const std::int64_t age = window_close_ms - bc.event_ts_ms;
    if (vector_stale_ms_ > 0 && age >= vector_stale_ms_) {
      ++stale_dropped;
      continue;
    }
    if (vector_two_sided_) {                 // ADR-052: собираем свежие кривые
      fresh_curves.push_back(bc.curve);
      continue;
    }
    double f = 1.0;
    if (vector_stale_ms_ > 0) {
      f = 1.0 - static_cast<double>(age) / static_cast<double>(vector_stale_ms_);
      if (f < 0.0) f = 0.0;
      if (f > 1.0) f = 1.0;
    }
    auto lv = vector_linear_segments_  // ADR-051: 1 линейный сегмент/сторону
                  ? LinearSegmentsFromCurve(bc.curve, vectorize_cfg_.decimal_scale)
                  : LevelsFromCurve(bc.curve, vectorize_cfg_.decimal_scale);
    for (auto& l : lv) {
      l.quantity = ScaleDecimal(l.quantity, f);
      l.remaining_quantity = ScaleDecimal(l.remaining_quantity, f);
      // ADR-051: для линейного сегмента масштабируем и d_hl, чтобы наклон
      // D=d_hl/q_max сохранялся (свежесть режет ёмкость и surplus, не наклон).
      l.d_hl_override = ScaleDecimal(l.d_hl_override, f);
      levels.push_back(std::move(l));
    }
  }

  // ADR-052/053: один двусторонний сегмент на венью над общим basis; safe-translator
  // берётся из curve.safe_translator() (venues посчитал из стакана). Иначе per-level/linear.
  domain::VectorizeResult vr =
      vector_two_sided_
          ? TwoSidedSegmentsFromCurves(fresh_curves, vectorize_cfg_.decimal_scale)
          : domain::Vectorize(levels, vectorize_cfg_);
  if (vr.segments.empty()) return;

  const std::string batch_id = "w|" + std::to_string(window_close_ms);
  if (vectorized_publisher_ != nullptr) {
    auto snap = transport::ToVectorizedSnapshot(vr, batch_id, window_close_ms,
                                                vectorize_cfg_.decimal_scale);
    vectorized_publisher_->Publish(snap);
  }
  if (vector_segment_storage_ != nullptr) {
    vector_segment_storage_->SaveSegments(batch_id, vr, window_close_ms);
  }
  // F-05A CE (вариант A): те же свежие кривые окна → book-derived агенты → ce.clearing.input.
  if (ce_agents_enabled_ && ce_publisher_ != nullptr && !fresh_curves.empty()) {
    BuildAndPublishCeClearingInput(fresh_curves, batch_id, window_close_ms);
  }
  cex::common::log_json("INFO", "F-05A vector window flushed",
                        {{"service", "market_data"},
                         {"stage", "vector_window"},
                         {"topic", "marketdata.vectorized"},
                         {"batch_id", batch_id},
                         {"venues_pairs", std::to_string(fresh.size())},
                         {"num_segments", std::to_string(vr.segments.size())},
                         {"num_assets", std::to_string(vr.basis.num_assets)},
                         {"stale_dropped", std::to_string(stale_dropped)}});
}

// F-05A CE (вариант A): свежие кривые окна → book-derived quote-агенты (agent_builder §A1)
// → CeClearingInput (agents + марки μ + P0) → ce.clearing.input. matching добавит плечи
// перевода/запаса из env и решит клиринг.
//
// ADR-064 (флаг CE_CROSS_PAIRS, default OFF): по умолчанию — только пары base/numeraire,
// как раньше (байт-в-байт регрессия). При CE_CROSS_PAIRS=1 переводчик обобщается на ЛЮБУЮ
// пару base/quote — обрабатываются и прямые кросс-пары (напр. ETH/BTC), чтобы агенты
// соответствовали реальным парам стаканов площадок, а не только X/номинал.
void MarketDataUseCases::BuildAndPublishCeClearingInput(
    const std::vector<fob::venue::v1::VenueLiquidityCurve>& curves,
    const std::string& batch_id, std::int64_t window_close_ms) {
  const char* num_env = std::getenv("CE_NUMERAIRE");
  const std::string numeraire = num_env ? std::string(num_env) : std::string("USDT");
  const char* theta_env = std::getenv("CE_AGENT_THETA");
  const double theta = theta_env ? std::atof(theta_env) : 0.5;
  // Множитель ½-спреда в полке агента (dead_zone). Дефолт 0 ⇒ полка нулевая
  // (решение владельца: переводчики клирят ликвидность книг, не простаивая на
  // полуспреде). Настраивается с фронта через env CE_DEAD_ZONE_HALF_SPREAD_MULT.
  const char* hsm_env = std::getenv("CE_DEAD_ZONE_HALF_SPREAD_MULT");
  const double half_spread_mult = hsm_env ? std::atof(hsm_env) : 0.0;
  // Порог широкого спреда (‰): тонкие/ненадёжные книги (кросс-пары с рассинхроном,
  // почти пустой стакан) сохраняют полный ½спред как полку. Дефолт 20‰.
  const char* wsg_env = std::getenv("CE_DEAD_ZONE_WIDE_SPREAD_PM");
  const double wide_spread_guard_pm = wsg_env ? std::atof(wsg_env) : 20.0;
  const bool cross_pairs = cex::common::Env::get_bool("CE_CROSS_PAIRS", false);
  auto to_dec = [](double x) {
    return cex::common::Decimal{static_cast<std::int64_t>(std::llround(x * 1e8)), 8}.to_proto();
  };

  struct Book {
    std::string base, quote, venue;
    double tau_ms{0.0};  // τ кривой (v = q/τ) — для согласования скорости с окном батча
    std::vector<domain::ExternalOrderLevel> levels;
  };
  std::vector<Book> books;
  // P0 (опорная цена в numeraire) считается ТОЛЬКО из пар base/numeraire —
  // кросс-пары (base/quote, quote≠numeraire) используют уже посчитанный P0.
  std::map<std::string, std::pair<double, int>> p0acc;  // base → (Σ mid, count)
  std::set<std::string> venues_set;
  std::vector<std::string> assets_order;
  std::set<std::string> assets_seen;
  for (const auto& c : curves) {
    const std::string base = c.instrument().base();
    const std::string quote = c.instrument().quote();
    const std::string venue = c.venue_id();
    if (base.empty() || venue.empty() || quote.empty()) continue;
    // CE_CROSS_PAIRS=0 (default): только base/numeraire — прежний фильтр, регрессия.
    if (!cross_pairs && quote != numeraire) continue;
    auto levels = LevelsFromCurve(c, vectorize_cfg_.decimal_scale);
    double bb = -1.0, ba = -1.0;
    for (const auto& l : levels) {
      const double p = static_cast<double>(l.price);
      if (p <= 0.0) continue;
      if (l.side == domain::LevelSide::kBid) bb = std::max(bb, p);
      else ba = (ba < 0.0) ? p : std::min(ba, p);
    }
    if (bb <= 0.0 || ba <= 0.0) continue;
    if (quote == numeraire) {  // P0_base — только из пар к номиналу (ADR-064)
      p0acc[base].first += 0.5 * (bb + ba);
      p0acc[base].second += 1;
    }
    venues_set.insert(venue);
    if (assets_seen.insert(base).second) assets_order.push_back(base);
    // Кросс-пара: quote тоже tradeable-актив (не numeraire) — регистрируем его
    // узел asset@venue в графе наравне с base (ADR-064 §Последствия).
    if (quote != numeraire && assets_seen.insert(quote).second) assets_order.push_back(quote);
    books.push_back({base, quote, venue, c.tau_ms(), std::move(levels)});
  }
  if (books.empty()) return;
  std::map<std::string, double> p0;
  for (const auto& [a, sc] : p0acc) p0[a] = sc.second > 0 ? sc.first / sc.second : 0.0;

  fob::marketdata::v1::CeClearingInput out;
  out.set_batch_id(batch_id);
  out.set_event_time_ms(window_close_ms);
  out.set_numeraire(numeraire);
  std::map<std::string, std::pair<double, double>> markacc;  // base → (Σ α·anchor, Σ α)
  // 1-й проход: строим валидных агентов, копим max глубину по активу.
  struct Ag { std::string base, quote, venue; double anchor, depth, dead_zone; };
  std::vector<Ag> agents;
  std::map<std::string, double> max_depth;
  int skipped_no_p0 = 0;
  // Согласование скорости торговли, окна батча и цен: кривая моделирует v = q/τ
  // (τ_eff калибрована по реальному обороту venue). CE-поток за такт = скорость·dt =
  // глубина·(dt/τ). Масштабируем α агента на batch_window/τ_кривой, чтобы позиция
  // менялась со скоростью реальной торговли биржи, а не «запаса» книги за такт.
  const bool ce_speed_scale = cex::common::Env::get_bool("CE_SPEED_SCALE", true);
  const double batch_window_ms =
      static_cast<double>(cex::common::Env::get_int("F05A_BATCH_WINDOW_MS", 1000));
  for (const auto& b : books) {
    const double p0_base = p0.count(b.base) ? p0.at(b.base) : 0.0;
    double reference_price = p0_base;
    if (b.quote != numeraire) {
      // Кросс-пара (ADR-064 §Решение): reference_price = P0_base/P0_quote —
      // согласовано с потенциалами узлов x[a@v]; для base/numeraire P0_quote=1,
      // reference_price=P0_base (без изменений). Без P0_quote пара недостроима.
      const double p0_quote = p0.count(b.quote) ? p0.at(b.quote) : 0.0;
      if (p0_base <= 0.0 || p0_quote <= 0.0) { ++skipped_no_p0; continue; }
      reference_price = p0_base / p0_quote;
    }
    domain::AgentBuilderConfig acfg;
    acfg.theta = theta;
    acfg.reference_price = reference_price;
    acfg.taker_fee_bps_override = ce_taker_fee_bps_;  // настраиваемая комиссия (0 ⇒ линейно)
    acfg.half_spread_mult = half_spread_mult;          // 0 ⇒ полка нулевая
    acfg.wide_spread_guard_pm = wide_spread_guard_pm;  // тонкие книги сохраняют ½спред
    const domain::QuoteAgent a = domain::BuildQuoteAgent(b.levels, acfg);
    if (!a.valid) continue;
    // Масштаб скорости: глубина(запас) → скорость·dt = глубина·(dt/τ). τ≤0 или
    // выключено ⇒ прежнее поведение (без масштаба, регрессия).
    double depth = a.depth;
    if (ce_speed_scale && b.tau_ms > 0.0 && batch_window_ms > 0.0) {
      double s = batch_window_ms / b.tau_ms;  // dt/τ
      if (s < 1e-6) s = 1e-6; else if (s > 100.0) s = 100.0;
      depth *= s;
    }
    agents.push_back({b.base, b.quote, b.venue, a.anchor_pm, depth, a.dead_zone_pm});
    // ADR-064: max глубина копится по ПАРЕ (base|quote), а не по base. Глубины
    // разных валют котировки несравнимы (ETH/BTC в BTC vs ETH/USDT в USDT) —
    // сравнение «тонкости» валидно только между площадками одной пары.
    const std::string pk = b.base + "|" + b.quote;
    if (depth > max_depth[pk]) max_depth[pk] = depth;
  }
  if (skipped_no_p0 > 0)
    cex::common::log_json("DEBUG", "CE clearing: кросс-пара без P0_quote пропущена",
                          {{"batch_id", batch_id}, {"skipped", std::to_string(skipped_no_p0)}});
  // Фильтр тонких/ненадёжных venue: глубина < ratio·max по активу (env CE_MIN_DEPTH_RATIO,
  // деф 0.05). Тонкие venue дают выбросные/устаревшие цены → fake-арбитраж → раздувание
  // позиции биржи (см. bug 2026-09-14: coinbase выброс −2.5‰ гнал BTC/SOL). ADR-057.
  const char* mdr = std::getenv("CE_MIN_DEPTH_RATIO");
  const double min_ratio = mdr ? std::atof(mdr) : 0.05;
  // Форс-кип venue (env CE_FORCE_KEEP_VENUES=comma) — оставить в клиринге даже при
  // тонкой глубине. Для DEX (uniswap_v3): честно расходящаяся AMM-цена — легитимный
  // источник CEX↔DEX арбитража, НЕ выброс (в отличие от тонкого coinbase, который
  // фильтр по-прежнему выкидывает). Решение владельца 2026-09-16.
  std::set<std::string> force_keep;
  if (const char* fk = std::getenv("CE_FORCE_KEEP_VENUES")) {
    std::string cur;
    for (const char* p = fk; ; ++p) {
      if (*p == ',' || *p == '\0') { if (!cur.empty()) force_keep.insert(cur); cur.clear(); if (*p == '\0') break; }
      else cur += *p;
    }
  }
  std::set<std::string> kept_venues;
  int dropped = 0;
  for (const auto& ag : agents) {
    // тонкий → искл., КРОМЕ форс-кип venue (реальный расходящийся источник арбитража).
    // ADR-064: порог считается по ПАРЕ (base|quote) — иначе кросс-пара (ETH/BTC)
    // всегда «тонкая» против глубокого ETH/USDT (разные валюты котировки).
    const std::string pk = ag.base + "|" + ag.quote;
    if (!force_keep.count(ag.venue) && ag.depth < min_ratio * max_depth[pk]) { ++dropped; continue; }
    auto* q = out.add_quotes();
    q->set_asset(ag.base);
    q->set_venue(ag.venue);
    *q->mutable_anchor() = to_dec(ag.anchor);
    *q->mutable_depth() = to_dec(ag.depth);
    *q->mutable_dead_zone() = to_dec(ag.dead_zone);
    // ADR-064: quote непуст только для кросс-пар; для номинальных пар оставляем
    // пустым (обратная совместимость — "пусто ⇒ numeraire").
    if (ag.quote != numeraire) q->set_quote(ag.quote);
    markacc[ag.base].first += ag.depth * ag.anchor;
    markacc[ag.base].second += ag.depth;
    kept_venues.insert(ag.venue);
  }
  if (out.quotes_size() == 0) return;
  for (const auto& a : assets_order) {
    out.add_assets(a);
    auto* m = out.add_marks();
    m->set_asset(a);
    const double mu = markacc[a].second > 0.0 ? markacc[a].first / markacc[a].second : 0.0;
    *m->mutable_mark() = to_dec(mu);
    *m->mutable_reference_price() = to_dec(p0[a]);
  }
  for (const auto& v : venues_set)
    if (kept_venues.count(v)) out.add_venues(v);  // только venue, пережившие фильтр
  if (dropped > 0)
    cex::common::log_json("INFO", "CE clearing: тонкие venue отфильтрованы",
                          {{"batch_id", batch_id}, {"dropped_agents", std::to_string(dropped)},
                           {"kept_quotes", std::to_string(out.quotes_size())},
                           {"min_depth_ratio", std::to_string(min_ratio)}});

  ce_publisher_->Publish(out);
  cex::common::log_json("INFO", "F-05A CE ce.clearing.input",
                        {{"service", "market_data"},
                         {"topic", "ce.clearing.input"},
                         {"batch_id", batch_id},
                         {"quotes", std::to_string(out.quotes_size())},
                         {"assets", std::to_string(out.assets_size())},
                         {"venues", std::to_string(out.venues_size())}});
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
