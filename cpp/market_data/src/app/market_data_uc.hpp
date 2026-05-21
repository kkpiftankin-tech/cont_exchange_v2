#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "fob/marketdata/v1/marketdata_raw.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "fob/venue/v1/venue.pb.h"

#include "domain/ports/i_liquidity_curve_storage.hpp"
#include "domain/ports/i_order_book_storage.hpp"
#include "update_order_book_uc.hpp"

// Forward declaration to avoid circular include
namespace cex::market_data::infra::clickhouse {
class ClickHouseLiquidityCurveStorage;
}

namespace cex::market_data::app {

// Port for persisting batch outputs in analytics storage (ClickHouse).
class IBatchOutputsStorage {
 public:
  virtual ~IBatchOutputsStorage() = default;
  virtual bool SaveBatchResult(const fob::matching::v1::BatchResult& evt) = 0;
  virtual bool SaveFills(const fob::matching::v1::BatchResult& evt) = 0;
};

class IExecutionVenueStorage {
 public:
  virtual ~IExecutionVenueStorage() = default;
  virtual bool SaveExecutionReport(const fob::execution::v1::ExecutionReport& evt) = 0;
};

// In-memory latest-ticker store + batch.outputs stats.
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
      infra::clickhouse::ClickHouseLiquidityCurveStorage* ch_curve_storage = nullptr);

  void OnMarketDataRaw(const fob::marketdata::v1::MarketDataRaw& evt);
  void OnBatchResult(const fob::matching::v1::BatchResult& batch);
  void OnExecutionReport(const fob::execution::v1::ExecutionReport& report);
  void OnLiquidityCurve(const fob::venue::v1::VenueLiquidityCurve& curve);
  void OnVenueSnapshot(const fob::venue::v1::VenueSnapshot& snapshot);

  std::optional<fob::marketdata::v1::Ticker> GetLastTicker(const std::string& venue,
                                                           const std::string& symbol) const;
  std::optional<fob::venue::v1::SideLiquidityCurve> GetLiquidityCurve(
      const std::string& venue, const std::string& symbol,
      fob::venue::v1::ExecutionSide side) const;

  BatchOutputsStats GetBatchOutputsStats() const;

 private:
  static std::string key(const std::string& venue, const std::string& symbol);
  static std::string curve_key(const std::string& venue, const std::string& symbol,
                               fob::venue::v1::ExecutionSide side);

  IBatchOutputsStorage* batch_storage_{nullptr};
  IExecutionVenueStorage* execution_storage_{nullptr};
  domain::ILiquidityCurveStorage* memory_curve_storage_{nullptr};
  infra::clickhouse::ClickHouseLiquidityCurveStorage* ch_curve_storage_{nullptr};
  mutable std::mutex mu_;
  std::unordered_map<std::string, fob::marketdata::v1::Ticker> last_ticker_;
  uint64_t batches_processed_{0};
  uint64_t fills_processed_{0};
  std::string last_batch_id_;

  UpdateOrderBookUseCase update_uc_;
};

}  // namespace cex::market_data::app
