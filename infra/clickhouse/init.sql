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
-- F-09: Batch/Combo/Multi-leg Orders (OLAP)
-- Sources: IN-011 §14.2; docs/07-data/olap-schema.md §F-09.
-- ADRs: ADR-032 (parent-child model), ADR-033 (execution.groups topic).
-- Ingestion:
--   execution.groups        → grouped_execution_events, grouped_ratio_deviation
--   fills (extended)        → grouped_leg_fills
--   computed / MV           → grouped_quality_metrics
--   backtest.execution.groups → grouped_replay_results
-- Monetary / qty fields: Decimal128(18) per CLAUDE.md §9.
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- grouped_execution_events: one row per ExecutionGroup event.
-- Source topic: execution.groups (producer: matching, key: parentOrderId).
-- ReplacingMergeTree deduplicates by (parent_order_id, execution_group_id,
-- event_time_ms) on compaction — idempotent re-ingestion safe.
-- Retention: 365 days (audit trail; same class as fills).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS grouped_execution_events (
    execution_group_id   String,
    batch_id             String,
    parent_order_id      String,
    user_id              String,
    combo_type           LowCardinality(String),
    execution_mode       LowCardinality(String),
    group_status         LowCardinality(String),
    atomicity_policy     LowCardinality(String),
    atomicity_scope      LowCardinality(String),
    fallback_action      LowCardinality(String),
    execution_scale      Decimal128(18),
    ratio_deviation_bps  Nullable(Int32),
    violated_constraints String,   -- JSON
    solver_diagnostics   String,   -- JSON: groupSolveTimeMs, bindingLegs[], bindingConstraints[]
    leg_count            UInt16,
    event_time_ms        Int64,
    ingested_at          DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(event_time_ms)
PARTITION BY toYYYYMMDD(toDateTime(intDiv(event_time_ms, 1000)))
ORDER BY (parent_order_id, execution_group_id, event_time_ms)
TTL toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL 365 DAY;

-- ---------------------------------------------------------------------------
-- grouped_leg_fills: per-leg fill with group context.
-- Source: extended fills topic (fields parentOrderId/executionGroupId/legId).
-- ReplacingMergeTree for idempotent re-ingestion.
-- Retention: 365 days.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS grouped_leg_fills (
    fill_id              String,
    execution_group_id   String,
    parent_order_id      String,
    leg_id               String,
    batch_id             String,
    user_id              String,
    instrument_symbol    LowCardinality(String),
    side                 LowCardinality(String),
    exec_qty             Decimal128(18),
    exec_price           Decimal128(18),
    exec_notional        Decimal128(18),
    group_policy         LowCardinality(String),
    liquidity_source     LowCardinality(String),
    venue_id             String,
    event_time_ms        Int64,
    ingested_at          DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(event_time_ms)
PARTITION BY toYYYYMMDD(toDateTime(intDiv(event_time_ms, 1000)))
ORDER BY (execution_group_id, leg_id, fill_id, event_time_ms)
TTL toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL 365 DAY;

-- ---------------------------------------------------------------------------
-- grouped_quality_metrics: quality aggregates per ExecutionGroup.
-- Populated via a Materialized View from grouped_execution_events +
-- grouped_leg_fills (combined VWAP, IS, ratio deviation).
-- ReplacingMergeTree for idempotent compaction.
-- Retention: 365 days.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS grouped_quality_metrics (
    execution_group_id        String,
    parent_order_id           String,
    batch_id                  String,
    user_id                   String,
    combo_type                LowCardinality(String),
    atomicity_policy          LowCardinality(String),
    group_status              LowCardinality(String),
    execution_scale           Decimal128(18),
    combined_vwap             Decimal128(18),
    combined_notional         Decimal128(18),
    combined_is_bps           Nullable(Int64),
    ratio_deviation_bps       Nullable(Int32),
    fallback_action           LowCardinality(String),
    leg_count                 UInt16,
    violated_constraint_count UInt16,
    solve_time_ms             UInt32,
    event_time_ms             Int64,
    ingested_at               DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(event_time_ms)
PARTITION BY toYYYYMMDD(toDateTime(intDiv(event_time_ms, 1000)))
ORDER BY (parent_order_id, execution_group_id, event_time_ms)
TTL toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL 365 DAY;

-- ---------------------------------------------------------------------------
-- grouped_ratio_deviation: per-leg per-batch ratio/weight deviation history.
-- Used to identify binding legs (AC-F09-002) and for risk/quality analytics.
-- MergeTree (no dedup needed; each (group, leg, time) triple is unique).
-- Retention: 180 days (shorter than events; deviation is less critical).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS grouped_ratio_deviation (
    execution_group_id    String,
    parent_order_id       String,
    batch_id              String,
    user_id               String,
    leg_id                String,
    instrument_symbol     LowCardinality(String),
    target_weight         Nullable(Decimal128(18)),
    target_ratio          Nullable(Decimal128(18)),
    actual_exec_qty       Decimal128(18),
    actual_exec_notional  Decimal128(18),
    deviation_bps         Int32,
    is_binding_leg        UInt8,
    event_time_ms         Int64,
    ingested_at           DateTime DEFAULT now()
)
ENGINE = MergeTree()
PARTITION BY toYYYYMMDD(toDateTime(intDiv(event_time_ms, 1000)))
ORDER BY (parent_order_id, event_time_ms, leg_id)
TTL toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL 180 DAY;

-- ---------------------------------------------------------------------------
-- grouped_replay_results: F-15 backtest/replay results for multi-leg orders.
-- Source: isolated topic backtest.execution.groups (ADR-015 isolation pattern,
-- analogous to backtest.execution.venue). NEVER mixed with live data.
-- Replay determinism: same replay_session_id + parent_order_id + group =>
-- same execution_scale (AC-F09-010).
-- ReplacingMergeTree for idempotent replay re-runs.
-- Retention: 90 days (replay data is transient vs live audit trail).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS grouped_replay_results (
    replay_session_id    String,
    execution_group_id   String,
    batch_id             String,
    parent_order_id      String,
    user_id              String,
    combo_type           LowCardinality(String),
    execution_mode       LowCardinality(String),
    group_status         LowCardinality(String),
    atomicity_policy     LowCardinality(String),
    execution_scale      Decimal128(18),
    ratio_deviation_bps  Nullable(Int32),
    combined_vwap        Nullable(Decimal128(18)),
    combined_notional    Nullable(Decimal128(18)),
    violated_constraints String,
    solver_diagnostics   String,
    leg_results          String,
    event_time_ms        Int64,
    ingested_at          DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(event_time_ms)
PARTITION BY toYYYYMMDD(toDateTime(intDiv(event_time_ms, 1000)))
ORDER BY (replay_session_id, parent_order_id, execution_group_id, event_time_ms)
TTL toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL 90 DAY;

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
-- F-09 / ADR-042: единый read-слой батчей (одномерные + многоногие combo).
--
-- batchresults (этот файл, выше) — иммутабельные single_leg батчи (F-04),
-- источник: matching → batch.outputs → market_data → ClickHouse.
--
-- combo (многоногие) батчи — это МУТАБЕЛЬНОЕ OLTP-состояние (статусы ног и
-- execution_scale меняются по циклам), их authoritative-хранилище — PostgreSQL
-- execution_groups. Чтобы не дублировать мутабельные строки в OLAP (риск
-- рассинхрона), федерируем их в ClickHouse через PostgreSQL table engine и
-- объединяем с batchresults во view batch_results_unified. Профиль (вкладка
-- «Батчи») и BFF /api/batches читают ЕДИНЫЙ источник — этот view.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS combo_groups_pg
(
    batch_id            String,
    parent_order_id     String,
    group_status        String,
    execution_scale     Decimal128(18),
    ratio_deviation_bps Nullable(Int32),
    leg_results         String,        -- JSON array [{legId, execQty, execPrice}]
    created_at          DateTime64(6)
)
ENGINE = PostgreSQL('postgres:5432', 'cex', 'execution_groups', 'cex', 'cex');

CREATE VIEW IF NOT EXISTS batch_results_unified AS
SELECT
    batch_id,
    event_time_ms,
    multiIf(residual_norm > 0.1, 'FAILED', residual_norm > 0.01, 'PARTIAL', 'SUCCESS') AS status,
    solve_time_ms,
    residual_norm,
    num_active_orders,
    fills_count,
    'single_leg' AS kind,
    '' AS parent_order_id,
    '' AS execution_scale,
    CAST(NULL AS Nullable(Int32)) AS ratio_deviation_bps
FROM batchresults
UNION ALL
SELECT
    batch_id,
    toUnixTimestamp64Milli(created_at) AS event_time_ms,
    multiIf(group_status = 'filled', 'SUCCESS',
            group_status IN ('failed','rejected','cancelled'), 'FAILED', 'PARTIAL') AS status,
    toFloat64(0) AS solve_time_ms,
    toFloat64(0) AS residual_norm,
    toInt64(JSONLength(leg_results)) AS num_active_orders,
    toInt64(arrayCount(x -> toFloat64OrZero(JSONExtractString(x, 'execQty')) > 0,
                       JSONExtractArrayRaw(leg_results))) AS fills_count,
    'combo_group' AS kind,
    parent_order_id,
    toString(execution_scale) AS execution_scale,
    ratio_deviation_bps
FROM combo_groups_pg;
