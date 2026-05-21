#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cex::backtest::app {

// Single row from `batchresults` table for replay loading.
struct HistoricalBatchResultRow {
  std::string batch_id;
  int64_t event_time_ms{0};
  std::string source;
  std::string correlation_id;
  std::string partition_key;
  double residual_norm{0};
  uint32_t solve_time_ms{0};
  uint32_t num_active_orders{0};
  uint32_t config_version{0};
  std::string solver_diagnostics_json;
  std::string clear_prices_json;
  std::string executed_rates_json;
  std::string used_liquidity_json;
  uint32_t fills_count{0};
};

// Single row from `fills` table for replay loading.
struct HistoricalFillRow {
  std::string batch_id;
  int64_t event_time_ms{0};
  std::string order_id;
  std::string user_id;
  std::string symbol;
  std::string base;
  std::string quote;
  std::string side;
  double executed_qty{0};
  double price{0};
  double executed_notional{0};
  double fee_amount{0};
  std::string fee_currency;
  std::string liquidity_source;
  std::string venue_id;
  std::string snapshot_id;
  std::string curve_id;
};

// Single row from `marketdata_snapshots` (aliased venue_snapshots) table.
struct HistoricalMarketdataSnapshotRow {
  std::string venue_id;
  std::string symbol;
  int64_t event_time_ms{0};
  double best_bid{0};
  double best_ask{0};
  double mid_price{0};
  double spread{0};
  std::string bid_depth_json;
  std::string ask_depth_json;
  double tick_size{0};
  double lot_size{0};
  std::string status;
};

// Single row from `risk_events` table for audit-mode diffing.
struct HistoricalRiskEventRow {
  std::string event_id;
  int64_t event_time_ms{0};
  std::string entity_id;
  std::string event_type;
  std::string order_id;
  std::string details_json;
  std::string batch_id;
};

// Port for chunked, deterministic loading of historical replay inputs from
// ClickHouse: batchresults, fills, marketdata_snapshots. Each Load* method must
// return rows ordered by (event_time_ms ASC, primary key ASC), starting at
// `offset` and capped at `limit`. `limit == 0` means "no further chunk".
class IHistoricalBatchLoader {
 public:
  virtual ~IHistoricalBatchLoader() = default;

  virtual std::vector<HistoricalBatchResultRow> LoadBatchResults(
      int64_t from_ms,
      int64_t to_ms,
      int64_t offset,
      int64_t limit) = 0;

  virtual std::vector<HistoricalFillRow> LoadFills(
      int64_t from_ms,
      int64_t to_ms,
      int64_t offset,
      int64_t limit) = 0;

  virtual std::vector<HistoricalMarketdataSnapshotRow> LoadMarketdataSnapshots(
      int64_t from_ms,
      int64_t to_ms,
      int64_t offset,
      int64_t limit) = 0;
};

// Single-batch audit-mode loader. This is intentionally separate from the
// chunked range loader because F15-METRIC-3 only needs by-id / by-timestamp
// lookups for one historical batch.
class IAuditHistoricalLoader {
 public:
  virtual ~IAuditHistoricalLoader() = default;

  virtual std::vector<HistoricalBatchResultRow> LoadBatchResultsById(
      const std::string& batch_id) = 0;
  virtual std::vector<HistoricalFillRow> LoadFillsByBatchIds(
      const std::vector<std::string>& batch_ids) = 0;
  virtual std::vector<HistoricalMarketdataSnapshotRow> LoadMarketdataSnapshotsByEventTimes(
      const std::vector<int64_t>& event_time_ms) = 0;
  virtual std::vector<HistoricalRiskEventRow> LoadRiskEventsByBatchId(
      const std::string& batch_id) = 0;
};

}  // namespace cex::backtest::app
