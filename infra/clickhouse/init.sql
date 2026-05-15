-- F-04 Batch Clearing: ClickHouse OLAP schema.
--
-- Mirrors the DDL emitted by ClickHouseBatchStorage::EnsureSchema in
-- origin/dev cpp/market_data/src/infra/clickhouse_storage.cpp so the
-- ingestion code (once imported) finds the tables already in place and
-- the read-side (Backtest, Replay, Web UI Diagnostics) has a stable
-- contract.
--
-- The ingestion path itself (Kafka batch.outputs / fills -> these tables)
-- ships with cpp/market_data update in a follow-up PR. Until then the
-- tables stay empty; matching writes to Kafka but no consumer drains it
-- into ClickHouse yet. AC-8 in F-04 is therefore still amber.

-- ---------------------------------------------------------------------------
-- batchresults: one row per RunBatch cycle. Used by F-04 Diagnostics screen
-- (residual_norm, solve_time_ms, num_active_orders) and by F-15 replay
-- parity checks. clear_prices/executed_rates are stored as JSON because
-- they are sparse maps over instruments.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS batchresults (
  batch_id                String,
  event_time_ms           Int64,
  source                  String,
  correlation_id          String,
  partition_key           String,
  residual_norm           Float64,
  solve_time_ms           UInt32,
  num_active_orders       UInt32,
  config_version          UInt32,
  solver_diagnostics_json String,
  clear_prices_json       String,
  executed_rates_json     String,
  used_liquidity_json     String,
  fills_count             UInt32,
  ingested_at             DateTime DEFAULT now()
) ENGINE = MergeTree
ORDER BY (event_time_ms, batch_id);

-- ---------------------------------------------------------------------------
-- fills: one row per FillEvent (a single order may have multiple rows in one
-- batch when multi-leg). Used for F-13 post-trade reports (VWAP / IS) and
-- F-06 historical PnL.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS fills (
  batch_id           String,
  event_time_ms      Int64,
  order_id           String,
  user_id            String,
  symbol             String,
  base               String,
  quote              String,
  side               LowCardinality(String),
  executed_qty       Float64,
  price              Float64,
  executed_notional  Float64,
  fee_amount         Float64,
  fee_currency       String,
  liquidity_source   String,
  venue_id           String,
  snapshot_id        String,
  curve_id           String,
  ingested_at        DateTime DEFAULT now()
) ENGINE = MergeTree
ORDER BY (event_time_ms, batch_id, order_id);
