#pragma once

#include <atomic>
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
#include "domain/vectorize.hpp"   // F-05A: VectorizeConfig (T-F05A-205)
#include "app/compute_market_data.hpp"
#include "update_order_book_uc.hpp"
#include "infra/market_data_stream_hub.hpp"
#include "infra/postgres/pg_market_data_config.hpp"

// Forward declaration to avoid circular include
namespace cex::market_data::infra::clickhouse {
class ClickHouseLiquidityCurveStorage;
}

namespace cex::market_data::app {

// F-05A (T-F05A-205): порт публикации векторизованной ликвидности.
struct IVectorizedPublisher;

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
      MarketDataConfig md_config = {});

  // --- существующие обработчики ---
  void OnMarketDataRaw(const fob::marketdata::v1::MarketDataRaw& evt);
  void OnBatchResult(const fob::matching::v1::BatchResult& batch);
  // F-09 observability: persist grouped combo execution to ClickHouse grouped_*.
  void OnExecutionGroup(const fob::matching::v1::ExecutionGroup& eg);
  void OnExecutionReport(const fob::execution::v1::ExecutionReport& report);
  void OnLiquidityCurve(const fob::venue::v1::VenueLiquidityCurve& curve);
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

  // F-05 ports
  domain::ISnapshotStorage*        snapshot_storage_{nullptr};
  domain::IEffectiveSpreadStorage* spread_storage_{nullptr};
  domain::ISnapshotPublisher*      snapshot_publisher_{nullptr};
  domain::IRiskAlertPublisher*     risk_publisher_{nullptr};
  IVectorizedPublisher*            vectorized_publisher_{nullptr};  // F-05A
  domain::VectorizeConfig          vectorize_cfg_{};                // F-05A
  infra::MarketDataStreamHub*      stream_hub_{nullptr};
  infra::PgMarketDataConfig*       pg_config_{nullptr};
  MarketDataConfig                 md_config_;

  std::atomic<bool> sweeper_running_{false};
  std::thread       sweeper_thread_;

  mutable std::mutex mu_;

  // Тикер-кэш (legacy, используется GetLastTicker)
  std::unordered_map<std::string, fob::marketdata::v1::Ticker> last_ticker_;

  // F-05: последний MarketDataSnapshot per asset (fast path для REST и GetReferencePrices)
  std::unordered_map<std::string, domain::MarketDataSnapshot> last_snapshot_;

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
