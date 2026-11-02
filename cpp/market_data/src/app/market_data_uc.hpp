#pragma once

#include "cex/common/data_validity.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "fob/marketdata/v1/marketdata_raw.pb.h"
#include "fob/marketdata/v1/marketdata_service.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "fob/orders/v1/orders.pb.h"
#include "fob/matching/v1/execution_group.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "fob/venue/v1/venue.pb.h"

#include "domain/entities/market_data_snapshot.hpp"
#include "domain/entities/flow_order_book.hpp"
#include "domain/ports/i_liquidity_curve_storage.hpp"
#include "domain/ports/i_order_book_storage.hpp"
#include "domain/ports/i_snapshot_storage.hpp"
#include "domain/ports/i_snapshot_publisher.hpp"
#include "domain/fill_depth_scale.hpp"  // ADR-082 шаг 1: снимок измерений f по биржам и парам
#include "domain/vectorize.hpp"   // F-05A: VectorizeConfig (T-F05A-205)
#include "fob/marketdata/v1/vector_liquidity.pb.h"  // F-05A (T-F05A-305 persister)
#include "app/compute_market_data.hpp"
#include "update_order_book_uc.hpp"
#include "infra/async_sink.hpp"  // CR-F18-016: запись истории кривых вне потока потребителя
#include "infra/market_data_stream_hub.hpp"
#include "infra/postgres/pg_market_data_config.hpp"

// Forward declaration to avoid circular include
namespace cex::market_data::infra::clickhouse {
class ClickHouseLiquidityCurveStorage;
}

// F-18 D1+#7 (T-F18-702): writer σ в ce_asset_volatility.
namespace cex::market_data::infra {
class PostgresAssetVolatilityRepository;
}

namespace cex::market_data::app {

// F-05A (T-F05A-205/206/305): порты публикации + персиста векторной ликвидности.
struct IVectorizedPublisher;
struct ICeClearingPublisher;  // F-05A CE (вариант A)
struct IVectorSegmentStorage;
struct IVectorClearingResultStorage;

// Port for persisting batch outputs in analytics storage (ClickHouse).
class IBatchOutputsStorage {
 public:
  virtual ~IBatchOutputsStorage() = default;
  virtual bool SaveBatchResult(const fob::matching::v1::BatchResult& evt) = 0;
  virtual bool SaveFills(const fob::matching::v1::BatchResult& evt) = 0;
  // F-09 observability: grouped combo execution → ClickHouse grouped_* OLAP.
  virtual bool SaveExecutionGroup(const fob::matching::v1::ExecutionGroup& evt) = 0;
};

class IExecutionVenueStorage {
 public:
  virtual ~IExecutionVenueStorage() = default;
  virtual bool SaveExecutionReport(const fob::execution::v1::ExecutionReport& evt) = 0;
};

// In-memory latest-ticker store + batch.outputs stats + F-05 snapshot cache.
class MarketDataUseCases {
 public:
  struct BatchOutputsStats {
    uint64_t batches_processed{0};
    uint64_t fills_processed{0};
    std::string last_batch_id;
  };

  explicit MarketDataUseCases(
      IBatchOutputsStorage* batch_storage = nullptr,
      IExecutionVenueStorage* execution_storage = nullptr,
      domain::IOrderBookStorage* ob_storage = nullptr,
      domain::IOrderBookPublisher* publisher = nullptr,
      domain::ILiquidityCurveStorage* memory_curve_storage = nullptr,
      infra::clickhouse::ClickHouseLiquidityCurveStorage* ch_curve_storage = nullptr,
      // F-05: snapshot и effective spread persistence + publishing
      domain::ISnapshotStorage* snapshot_storage = nullptr,
      domain::IEffectiveSpreadStorage* spread_storage = nullptr,
      domain::ISnapshotPublisher* snapshot_publisher = nullptr,
      domain::IRiskAlertPublisher* risk_publisher = nullptr,
      infra::MarketDataStreamHub* stream_hub = nullptr,
      infra::PgMarketDataConfig* pg_config = nullptr,
      IVectorizedPublisher* vectorized_publisher = nullptr,  // F-05A (T-F05A-205)
      MarketDataConfig md_config = {},
      ICeClearingPublisher* ce_publisher = nullptr);          // F-05A CE (вариант A)

  // --- существующие обработчики ---
  void OnMarketDataRaw(const fob::marketdata::v1::MarketDataRaw& evt);
  void OnBatchResult(const fob::matching::v1::BatchResult& batch);
  // F-09 observability: persist grouped combo execution to ClickHouse grouped_*.
  void OnExecutionGroup(const fob::matching::v1::ExecutionGroup& eg);
  void OnExecutionReport(const fob::execution::v1::ExecutionReport& report);
  void OnLiquidityCurve(const fob::venue::v1::VenueLiquidityCurve& curve);

  // F-05A (T-F05A-206): опциональный персист векторных сегментов (ClickHouse).
  void SetVectorSegmentStorage(IVectorSegmentStorage* storage) {
    vector_segment_storage_ = storage;
  }

  // F-05A (T-F05A-305 persister): consume matching.vector_clearing → CH.
  void SetVectorClearingResultStorage(IVectorClearingResultStorage* storage) {
    vector_clearing_result_storage_ = storage;
  }

  // F-18 D1+#7 (T-F18-702): включить оценку σ (EWMA волатильности mid) + запись в
  // ce_asset_volatility. halflife_sec — полупериод time-aware EWMA. write_period_sec
  // — троттлинг записи в PG на актив.
  void SetAssetVolatilityRepository(infra::PostgresAssetVolatilityRepository* repo,
                                    double halflife_sec, double write_period_sec) {
    vol_repo_ = repo;
    if (halflife_sec > 0.0) vol_halflife_sec_ = halflife_sec;
    if (write_period_sec > 0.0) vol_write_period_sec_ = write_period_sec;
  }
  // ADR-082 шаг 1: масштабировать глубину котировок агентов коэффициентом исполнения биржи f. Пустая функция (деф) ⇒ глубина
  // как раньше, ноль изменений. source отдаёт снимок измерений (не должен блокироваться на базе: читатель держит его в памяти).
  // cfg: f_min — нижняя граница множителя, n0 — псевдо-заявки с f=1 в сжатии к единице, ref_min — нижняя граница опорного f
  // при нормировке на лучшую биржу актива.
  void SetVenueFillSource(std::function<domain::VenueFillSnapshot()> source, const domain::FillDepthScaleConfig& cfg) {
    fill_source_ = std::move(source);
    fill_depth_cfg_ = cfg;
  }
  void OnVectorClearingResult(
      const fob::marketdata::v1::VectorClearingResult& result);
  void OnVenueSnapshot(const fob::venue::v1::VenueSnapshot& snapshot);

  // F-05: обработчик FillEvent для расчёта effective spread
  void OnFillEvent(const fob::matching::v1::FlowFill& fill,
                   const std::string& fill_id,
                   const std::string& batch_id,
                   domain::Timestamp ts);

  // F5-3: обработка FlowOrder событий для построения aggregate curves
  void OnOrderCreate(const fob::orders::v1::FlowOrder& order);
  void OnOrderCancel(const std::string& order_id, const std::string& asset);

  // --- существующие геттеры ---
  std::optional<fob::marketdata::v1::Ticker> GetLastTicker(const std::string& venue,
                                                           const std::string& symbol) const;
  std::optional<fob::venue::v1::SideLiquidityCurve> GetLiquidityCurve(
      const std::string& venue, const std::string& symbol,
      fob::venue::v1::ExecutionSide side) const;

  BatchOutputsStats GetBatchOutputsStats() const;

  // F-05: текущий snapshot из in-memory кэша
  std::optional<domain::MarketDataSnapshot> GetMarketDataSnapshot(
      const std::string& asset) const;

  // F-05: reference prices для Matching Backend (batch-чтение)
  std::vector<domain::MarketDataSnapshot> GetReferencePrices(
      const std::vector<std::string>& assets) const;

  // F-05: текущий mid для конкретного asset (используется в ComputeEffectiveSpread)
  std::optional<common::Decimal> GetCurrentMid(const std::string& asset) const;

  // Запускает background поток, который помечает устаревшие снимки stale: true
  void StartStaleSweeper();
  void StopStaleSweeper();

  // F-05A ADR-050: оконный агрегатор клиринга. Таймер-поток по F05A_BATCH_WINDOW_MS
  // собирает буфер свежих кривых (все venue×pair) в ОДИН marketdata.vectorized
  // над общим asset-basis. No-op, если F05A_BATCH_WINDOW_ENABLED выключен.
  void StartVectorWindow();
  void StopVectorWindow();
  // Собрать окно и опубликовать один агрегированный снапшот (public для тестов).
  void FlushVectorWindow(std::int64_t window_close_ms);
  // F-05A CE (вариант A): собрать book-derived агентов из свежих кривых окна и
  // опубликовать CeClearingInput в ce.clearing.input (agent_builder §A1).
  // ADR-075 (этап 4, причина 2): curve_ts_ms — время события каждой кривой, ключ "venue|SYMBOL"
  // (SYMBOL как в instrument().symbol(), напр. "ETH/USDT"); по нему считается возраст снимка
  // для age-aware полки c += k·σ·√age (env CE_AGE_DEAD_ZONE_K, деф 0 ⇒ член выключен).
  void BuildAndPublishCeClearingInput(
      const std::vector<fob::venue::v1::VenueLiquidityCurve>& curves,
      const std::string& batch_id, std::int64_t window_close_ms,
      const std::map<std::string, std::int64_t>& curve_ts_ms = {},
      const std::vector<fob::marketdata::v1::CeDroppedQuote>& stale_dropped = {});

 private:
  static std::string key(const std::string& venue, const std::string& symbol);
  static std::string curve_key(const std::string& venue, const std::string& symbol,
                               fob::venue::v1::ExecutionSide side);

  // Обновляет in-memory кэш, сохраняет в ClickHouse, публикует через Kafka/WS.
  void ProcessSnapshot(domain::MarketDataSnapshot snap);

  IBatchOutputsStorage* batch_storage_{nullptr};
  IExecutionVenueStorage* execution_storage_{nullptr};
  domain::ILiquidityCurveStorage* memory_curve_storage_{nullptr};
  infra::clickhouse::ClickHouseLiquidityCurveStorage* ch_curve_storage_{nullptr};
  // CR-F18-016: историю кривых в ClickHouse пишет отдельный поток (ограниченная очередь, вытесняется старейшее);
  // пусто ⇒ запись синхронно в потоке потребителя (MARKET_DATA_CH_ASYNC=0 или нет хранилища).
  std::unique_ptr<infra::AsyncSink<fob::venue::v1::VenueLiquidityCurve>> curve_sink_;
  std::int64_t last_empty_window_warn_ms_{-60000};  ///< троттлинг предупреждения «окно пропущено» (поток окна)

  // F-05 ports
  domain::ISnapshotStorage*        snapshot_storage_{nullptr};
  domain::IEffectiveSpreadStorage* spread_storage_{nullptr};
  domain::ISnapshotPublisher*      snapshot_publisher_{nullptr};
  domain::IRiskAlertPublisher*     risk_publisher_{nullptr};
  IVectorizedPublisher*            vectorized_publisher_{nullptr};  // F-05A
  ICeClearingPublisher*            ce_publisher_{nullptr};          // F-05A CE (вариант A)
  bool                             ce_agents_enabled_{false};       // env CE_AGENTS_ENABLED
  IVectorSegmentStorage*           vector_segment_storage_{nullptr};// F-05A (T-F05A-206)
  IVectorClearingResultStorage*    vector_clearing_result_storage_{nullptr}; // F-05A
  domain::VectorizeConfig          vectorize_cfg_{};                // F-05A
  infra::MarketDataStreamHub*      stream_hub_{nullptr};
  infra::PgMarketDataConfig*       pg_config_{nullptr};
  MarketDataConfig                 md_config_;

  std::atomic<bool> sweeper_running_{false};
  std::thread       sweeper_thread_;

  // F-05A ADR-050: batch-window aggregation state.
  bool               vector_window_enabled_{false};
  bool               vector_linear_segments_{false};  ///< ADR-051: 1 линейный сегм./сторону
  bool               vector_two_sided_{false};         ///< ADR-052: 1 двусторонний сегм./венью
  std::int64_t       vector_window_ms_{1000};   ///< F05A_BATCH_WINDOW_MS (runtime PG override)
  std::int64_t       vector_stale_ms_{2000};    ///< F05A_STALE_LEVEL_MS (runtime PG override)
  double             ce_taker_fee_bps_{0.0};    ///< комиссия тейкера (bps) из f05a_clearing_config;
                                                ///< по умолчанию 0 = линейные кривые (владелец 2026-09-16);
                                                ///< <0 = из стакана venue (выставляется с фронта)
  // ADR-050/052: runtime-конфиг окна из PG (таблица f05a_clearing_config), с TTL-кэшем.
  std::string        clearing_cfg_pg_conn_{};
  std::int64_t       clearing_cfg_last_read_ms_{0};  ///< monotonic ms последнего чтения PG
  void RefreshClearingConfigFromPg();                ///< обновляет window/stale из PG (TTL)
  std::atomic<bool>  vector_window_running_{false};
  std::thread        vector_window_thread_;
  std::mutex         vector_window_mu_;
  struct BufferedCurve {
    fob::venue::v1::VenueLiquidityCurve curve;
    std::int64_t event_ts_ms{0};   ///< время события кривой (для веса свежести)
  };
  // key = venue|pair; новая кривая вытесняет старую (ADR-050 §Решение).
  std::unordered_map<std::string, BufferedCurve> vector_window_buffer_;

  mutable std::mutex mu_;

  // Тикер-кэш (legacy, используется GetLastTicker)
  std::unordered_map<std::string, fob::marketdata::v1::Ticker> last_ticker_;

  // F-05: последний MarketDataSnapshot per asset (fast path для REST и GetReferencePrices)
  std::unordered_map<std::string, domain::MarketDataSnapshot> last_snapshot_;

  // F-18 D1+#7 (T-F18-702): time-aware EWMA дисперсии лог-доходностей mid per актив.
  // σ = sqrt(ewma_var) [за √сек]; ledger считает Γ=γσ²τ. var_persec = r²/Δt_sec,
  // ewma_var = w·ewma_var + (1−w)·var_persec, w = exp(−Δt/halflife). Запись в PG
  // троттлится vol_write_period_sec на актив. Обновляется под mu_ в ProcessSnapshot.
  struct VolState {
    double prev_mid{0.0};
    double ewma_var{0.0};       // EWMA дисперсии лог-доходности за сек
    long long samples{0};
    long long last_ms{0};        // t предыдущего mid (для Δt)
    long long last_write_ms{0};  // троттлинг записи в PG
    bool init{false};
  };
  // Отдельный мьютекс σ-состояния (ревью ADR-075): mu_ держится на синхронном I/O (ClickHouse,
  // Kafka) в OnMarketDataRaw, поэтому поток окна не должен вставать за ним ради чтения σ.
  // Порядок захвата: mu_ → vol_mu_ (SigmaSnapshot берёт ТОЛЬКО vol_mu_). Обратного нет.
  mutable std::mutex vol_mu_;
  std::unordered_map<std::string, VolState> vol_state_;  // под vol_mu_
  infra::PostgresAssetVolatilityRepository* vol_repo_{nullptr};
  std::function<domain::VenueFillSnapshot()> fill_source_;  // ADR-082 шаг 1; пусто ⇒ выключено
  domain::FillDepthScaleConfig fill_depth_cfg_{};
  double vol_halflife_sec_{60.0};
  double vol_write_period_sec_{15.0};
  // Обновляет EWMA-σ по новому mid (ПОД mu_). Возвращает true, если по троттлингу
  // пора записать в PG; тогда *out_sigma/*out_samples заполнены. Саму запись
  // ProcessSnapshot делает ВНЕ лока (PG не держит mu_).
  bool UpdateAssetVolatilityLocked(const std::string& asset, double mid,
                                   long long now_ms, double* out_sigma,
                                   long long* out_samples);
  // Копия σ (на √сек, лог-доходность) по символам пар ("ETHUSDT") с ≥ min_samples выборок.
  // Берёт ТОЛЬКО vol_mu_ (коротко, раз на окно) — не mu_, на котором висит I/O консьюмера.
  std::map<std::string, double> SigmaSnapshot(long long min_samples) const;

  // ADR-078 §7: σ по потоку mid КАЖДОЙ площадки отдельно ("venue|ETHUSDT"). Объединённый ряд актива
  // (SigmaSnapshot) смешивает mid разных бирж, и межбиржевой базис попадает в r²/Δt: на dev σ BTC по объединённому
  // потоку 0,106 ‰/√с против 0,042 по каждой бирже. Срок годности КРИВОЙ определяется движением её собственной
  // площадки, поэтому жёсткий порог устаревания берёт σ отсюда. Берёт ТОЛЬКО vol_mu_.
  void UpdateVenueVolatility(const std::string& venue_symbol_key, double mid, double t_s);
  std::map<std::string, double> VenueSigmaSnapshot(long long min_samples) const;
  std::unordered_map<std::string, cex::common::DualHorizonVolatility> venue_vol_;  // под vol_mu_

  // F-05 perf: троттлинг ClickHouse в hot-path консьюмера. На каждый снапшот
  // (включая высокочастотные venue.snapshots) синхронный CH-запрос перегружал
  // ClickHouse и блокировал консьюмер → internal-снапшоты не успевали. Кэшируем
  // volume24h и пишем в CH не чаще раза в N секунд на asset; in-memory кэш
  // (last_snapshot_) обновляется всегда.
  std::unordered_map<std::string, common::Decimal>  volume24h_cache_;
  std::unordered_map<std::string, domain::Timestamp> last_ch_persist_;

  // F5-3: FOB aggregate curves — хранит активные FlowOrder для bestBid/bestAsk
  domain::FlowOrderBook flow_order_book_;

  uint64_t batches_processed_{0};
  uint64_t fills_processed_{0};
  std::string last_batch_id_;

  UpdateOrderBookUseCase update_uc_;
};

}  // namespace cex::market_data::app
