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

-- ---------------------------------------------------------------------------
-- F-12 Execution Hedge: execution_reports analytics table.
-- Sources: IN-005 v1 + IN-008 v2 (resolves DoD-7 in F-12 feature.yaml).
--
-- One row per ExecutionReport (Kafka topic execution.venue). Used by:
-- - Hedge PnL Dashboard (DoD-15) for clearingPrice vs avgFillPrice
--   scatter, slippage histogram, PnL over time.
-- - Reconciliation Alerts to surface UNDERFILLED hedge flows.
-- - F-15 backtest parity checks: replay-mode reports tagged with
--   `replay::<session_id>` namespace use the same table OR a sibling
--   `backtest_execution_reports` per BACKTEST_CLICKHOUSE_EXECUTION_VENUE_TABLE
--   (kept analogous to BUG-2 part 2 fix in IN-007).
--
-- Retention: TTL set on event_time_ms - keep 90 days of execution history,
-- per F-12 non-functional requirement (Section 2.1, "Хранение execution reports
-- в ClickHouse с retention ≥ 90 дней").
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS execution_reports (
  report_id         String,
  intent_id         String,
  hedge_flow_id     String,
  child_order_id    String,
  batch_id          String,
  provider_id       String,
  venue_id          LowCardinality(String),
  symbol            String,
  side              LowCardinality(String),
  status            LowCardinality(String),
  filled_qty        Float64,
  remaining_qty     Float64,
  avg_price         Float64,
  fee_amount        Float64,
  fee_currency      String,
  slippage_bps      Int32,
  reference_mid     Float64,
  hedge_pnl         Float64,
  event_time_ms     Int64,
  ingested_at       DateTime DEFAULT now()
) ENGINE = MergeTree
PARTITION BY toYYYYMM(toDateTime(event_time_ms / 1000))
ORDER BY (symbol, venue_id, event_time_ms, report_id)
TTL toDateTime(event_time_ms / 1000) + INTERVAL 90 DAY;

-- Projection for HedgeFlow Monitor drill-down: queries by hedge_flow_id are
-- O(N) without this projection because primary key starts with symbol.
ALTER TABLE execution_reports
  ADD PROJECTION IF NOT EXISTS prj_by_hedge_flow (
    SELECT * ORDER BY (hedge_flow_id, event_time_ms)
  );

-- ===========================================================================
-- F-20 Live Venue Simulator (PR-F20-4). OLAP history for sim execution +
-- SHADOW divergence. ADR-015: this is fed from `sim.execution.venue` (sim
-- ExecutionReport, identical to the live contract) plus the
-- `sim.execution.annotations` sidecar (SimExecutionAnnotation). There is NO
-- `sim_mode` column — every row in this table is sim by construction (it
-- only exists because the row arrived on a sim topic).
-- ===========================================================================

-- sim_execution_reports: the F-12 ExecutionReport fields (same columns as
-- the live `execution_reports` table) PLUS the SimExecutionAnnotation
-- columns (lob_snapshot_id / lob_age_ms / impact_bps / latency_sample_ms),
-- joined by report_id at ingest time by the sim CH writer.
CREATE TABLE IF NOT EXISTS sim_execution_reports (
  -- --- shared ExecutionReport columns (mirror execution_reports) ---
  report_id          String,
  intent_id          String,
  hedge_flow_id      String,
  child_order_id     String,
  batch_id           String,
  provider_id        String,
  venue_id           LowCardinality(String),
  symbol             String,
  side               LowCardinality(String),
  status             LowCardinality(String),
  filled_qty         Float64,
  remaining_qty      Float64,
  avg_price          Float64,
  fee_amount         Float64,
  fee_currency       String,
  slippage_bps       Int32,
  reference_mid      Float64,
  hedge_pnl          Float64,
  event_time_ms      Int64,
  -- --- SimExecutionAnnotation sidecar columns (ADR-015) ---
  sim_session_id     String,
  lob_snapshot_id    String,
  lob_age_ms         UInt32,
  impact_bps         Float64,
  latency_sample_ms  UInt32,
  ingested_at        DateTime DEFAULT now()
) ENGINE = MergeTree
PARTITION BY toYYYYMM(toDateTime(event_time_ms / 1000))
ORDER BY (sim_session_id, symbol, venue_id, event_time_ms, report_id)
TTL toDateTime(event_time_ms / 1000) + INTERVAL 90 DAY;

-- sim_divergence_log: SHADOW-mode comparison of LIVE vs SIM execution for
-- the same clientOrderId. Written by the Divergence Service after it pairs
-- the two ExecutionReports (one from execution.venue, one from
-- sim.execution.venue). Per F20-10 / DoD-9.
CREATE TABLE IF NOT EXISTS sim_divergence_log (
  divergence_id      String,
  sim_session_id     String,
  client_order_id    String,
  hedge_flow_id      String,
  venue_id           LowCardinality(String),
  symbol             String,
  live_filled_qty    Float64,
  sim_filled_qty     Float64,
  delta_fill_qty     Float64,
  live_avg_price     Float64,
  sim_avg_price      Float64,
  delta_price_bps    Float64,
  live_latency_ms    UInt32,
  sim_latency_ms     UInt32,
  delta_latency_ms   Int32,
  live_fee           Float64,
  sim_fee            Float64,
  delta_fee          Float64,
  event_time_ms      Int64,
  ingested_at        DateTime DEFAULT now()
) ENGINE = MergeTree
PARTITION BY toYYYYMM(toDateTime(event_time_ms / 1000))
ORDER BY (sim_session_id, event_time_ms, client_order_id)
TTL toDateTime(event_time_ms / 1000) + INTERVAL 90 DAY;

-- ---------------------------------------------------------------------------
-- F-05 Live Market Data: marketdata_snapshots
-- One row per MarketDataSnapshot published after each BatchResult or
-- external venue update. Used by REST history API and GetReferencePrices.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS marketdata_snapshots (
  snapshot_id       String,
  asset             String,
  mid               String,
  best_bid          String,
  best_ask          String,
  spread            String,
  spread_bps        String,
  volume_24h        String,
  volume_quote_24h  String,
  bid_depth         String,   -- JSON [{price, qty}, ...]
  ask_depth         String,
  clear_price       String,
  executed_rate     String,
  source            String,   -- internal | cex | dex | composite
  stale             UInt8 DEFAULT 0,
  batch_id          String,
  timestamp         DateTime64(3, 'UTC')
) ENGINE = MergeTree
ORDER BY (asset, timestamp)
TTL toDateTime(timestamp) + INTERVAL 90 DAY
SETTINGS index_granularity = 8192;

-- ---------------------------------------------------------------------------
-- F-05 Live Market Data: effective_spreads
-- One row per FillEvent: post-trade effective spread for TCA / F-13.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS effective_spreads (
  fill_id               String,
  asset                 String,
  exec_price            String,
  mid_at_exec           String,
  effective_spread      String,
  effective_spread_bps  String,
  batch_id              String,
  timestamp             DateTime64(3, 'UTC')
) ENGINE = MergeTree
ORDER BY (asset, timestamp)
TTL toDateTime(timestamp) + INTERVAL 365 DAY
SETTINGS index_granularity = 8192;
