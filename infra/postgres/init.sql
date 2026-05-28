-- F-04 Batch Clearing: PostgreSQL OLTP schema.
--
-- Schema is consumed by cpp/matching:
--   - PostgresFlowOrderRepository (reads + updates flow_orders / flow_order_legs)
--   - PostgresSolverConfigRepository (reads solver_config)
--
-- Field choices match the SQL queries inside those repositories and the
-- proto contract fob.matching.v1 / fob.orders.v1 / fob.common.v1 (Decimal,
-- TimeInForce, OrderStatus). Decimal values are stored as PostgreSQL
-- NUMERIC and converted to fob.common.v1.Decimal via decimal_conversion.cpp.
--
-- This is the minimum schema for matching to read from Postgres. Inserts
-- into flow_orders/flow_order_legs come from order_flow service; that
-- producer wiring is a separate task (T-F04-002 / out of scope of this PR).

CREATE EXTENSION IF NOT EXISTS pgcrypto;

-- ---------------------------------------------------------------------------
-- flow_orders: active client FlowOrders the matching solver picks up each
-- batch.
--
-- Lifecycle: new -> active -> partially_filled -> filled
--                            -> cancelled / rejected / expired
--
-- Indexed by status + window so that the periodic
--   "SELECT ... WHERE status IN ('active','partially_filled')
--               AND filled_cum < q_max
--               AND window_start <= NOW() < window_end"
-- scan in PostgresFlowOrderRepository::LoadActiveOrders() stays fast.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS flow_orders (
  order_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id         TEXT NOT NULL,
  -- price range and rate (NUMERIC mirrors fob.common.v1.Decimal precision).
  p_low           NUMERIC(38, 18) NOT NULL,
  p_high          NUMERIC(38, 18) NOT NULL,
  q_rate          NUMERIC(38, 18) NOT NULL,
  q_max           NUMERIC(38, 18) NOT NULL,
  filled_cum      NUMERIC(38, 18) NOT NULL DEFAULT 0,
  -- TIF enum: 'GTC' | 'GTD' | 'IOC' (lowercase accepted by the parser too).
  time_in_force   TEXT NOT NULL,
  -- OrderStatus enum: 'new' | 'active' | 'partially_filled' | 'filled' |
  --                   'cancelled' | 'rejected' | 'expired'
  status          TEXT NOT NULL,
  -- Execution window the order is valid in.
  window_start    TIMESTAMPTZ NOT NULL,
  window_end      TIMESTAMPTZ,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),

  CONSTRAINT flow_orders_q_max_positive   CHECK (q_max > 0),
  CONSTRAINT flow_orders_q_rate_positive  CHECK (q_rate > 0),
  CONSTRAINT flow_orders_filled_cum_valid CHECK (filled_cum >= 0 AND filled_cum <= q_max),
  CONSTRAINT flow_orders_price_range      CHECK (p_low > 0 AND p_high >= p_low),
  CONSTRAINT flow_orders_window_valid     CHECK (window_end IS NULL OR window_end > window_start)
);

CREATE INDEX IF NOT EXISTS idx_flow_orders_active_window
  ON flow_orders (status, window_start, window_end)
  WHERE status IN ('active', 'partially_filled');

CREATE INDEX IF NOT EXISTS idx_flow_orders_user_id
  ON flow_orders (user_id);

-- ---------------------------------------------------------------------------
-- flow_order_legs: per-asset weights for portfolio / multi-leg FlowOrders.
--
-- A FlowOrder with a single leg is a single-asset order (e.g. BTCUSDT).
-- Multi-leg orders carry one row per asset with a weight (matching the
-- portfolio semantics from F-04 / F-09).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS flow_order_legs (
  order_id          UUID NOT NULL REFERENCES flow_orders(order_id) ON DELETE CASCADE,
  instrument_symbol TEXT NOT NULL,
  weight            NUMERIC(38, 18) NOT NULL,
  PRIMARY KEY (order_id, instrument_symbol)
);

CREATE INDEX IF NOT EXISTS idx_flow_order_legs_symbol
  ON flow_order_legs (instrument_symbol);

-- ---------------------------------------------------------------------------
-- solver_config: tunable parameters for the matching solver.
-- Only one row at a time should have isactive = true (enforced by partial
-- unique index below). Matching reads the active row on each batch start.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS solver_config (
  version            INTEGER PRIMARY KEY,
  batchintervalms    INTEGER NOT NULL,
  maxiterations      INTEGER NOT NULL,
  epsilonliquidity   DOUBLE PRECISION NOT NULL,
  tolerance          DOUBLE PRECISION NOT NULL,
  feemodel           JSONB NOT NULL DEFAULT '{}'::jsonb,
  isactive           BOOLEAN NOT NULL DEFAULT FALSE,
  created_at         TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE UNIQUE INDEX IF NOT EXISTS solver_config_one_active
  ON solver_config (isactive)
  WHERE isactive = TRUE;

-- Seed an MVP default configuration. Values match docs/02-system/features/
-- F-04-batch-clearing/feature.yaml SLA section: median solve_time_ms <= 500
-- so batchintervalms is set to 1000 (1 batch / sec). Tolerance and
-- maxiterations are conservative defaults for the Eigen Sparse Cholesky
-- solver and can be tuned per environment.
INSERT INTO solver_config (
  version, batchintervalms, maxiterations, epsilonliquidity, tolerance,
  feemodel, isactive
)
VALUES (
  1, 1000, 128, 0.00001, 0.0001,
  '{"makerfeerate":0.0002,"takerfeerate":0.0005}'::jsonb, TRUE
)
ON CONFLICT (version) DO NOTHING;

-- ============================================================================
-- F-12 Execution Hedge: OLTP tables
-- Sources: IN-005 v1 + IN-008 v2 (see docs/02-system/features/F-12-execution-hedge/)
-- Resolves DoD-8 in feature.yaml#definitionOfDone.
-- ============================================================================

-- hedgeflows: one row per ExecutionIntent / HedgeFlow session.
-- Authoritative source of state while status='OPEN'. UI HedgeFlow Monitor
-- (DoD-14) reads from this table.
CREATE TABLE IF NOT EXISTS hedgeflows (
  -- IDs are TEXT (not UUID) to accommodate matching's composite intent_id
  -- format "<batch>|<order>|<symbol>|<venue>|external_fill_N" used by F-04
  -- external fill intents. Real F-12 hedge intents (from
  -- hedge_trigger_policy) carry a UUID — but both end up here.
  hedge_flow_id    TEXT PRIMARY KEY,
  intent_id        TEXT NOT NULL,
  batch_id         TEXT,                       -- nullable: manual override has no batch
  provider_id      TEXT NOT NULL,
  symbol           TEXT NOT NULL,
  side             TEXT NOT NULL CHECK (side IN ('BUY', 'SELL')),
  target_qty       NUMERIC(38, 18) NOT NULL CHECK (target_qty > 0),
  filled_qty       NUMERIC(38, 18) NOT NULL DEFAULT 0 CHECK (filled_qty >= 0),
  target_notional  NUMERIC(38, 18),
  reference_mid    NUMERIC(38, 18),
  avg_fill_price   NUMERIC(38, 18),
  tot_fee          NUMERIC(38, 18) NOT NULL DEFAULT 0,
  hedge_pnl        NUMERIC(38, 18),             -- computed by Settlement Ledger
  urgency          TEXT NOT NULL CHECK (urgency IN ('LOW', 'MEDIUM', 'HIGH')),
  timeout_ms       INTEGER NOT NULL CHECK (timeout_ms > 0),
  status           TEXT NOT NULL CHECK (status IN ('OPEN', 'COMPLETED', 'UNDERFILLED', 'REJECTED', 'RISK_REJECTED', 'CANCELLED')),
  error_code       TEXT,
  error_message    TEXT,
  created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  completed_at     TIMESTAMPTZ,
  CONSTRAINT hedgeflows_filled_le_target CHECK (filled_qty <= target_qty * 1.01)  -- 1% overfill tolerance
);

CREATE INDEX IF NOT EXISTS idx_hedgeflows_provider_symbol ON hedgeflows (provider_id, symbol);
CREATE INDEX IF NOT EXISTS idx_hedgeflows_batch_id ON hedgeflows (batch_id) WHERE batch_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_hedgeflows_status_open ON hedgeflows (status, updated_at) WHERE status IN ('OPEN');
CREATE INDEX IF NOT EXISTS idx_hedgeflows_status_underfilled ON hedgeflows (status, created_at) WHERE status IN ('UNDERFILLED', 'REJECTED');

-- child_orders: one row per actual order placed on a venue (CEX/DEX/AMM).
-- Multi-venue routing decomposes single HedgeFlow into N child_orders.
-- clientOrderId is the idempotency key sent to the venue.
CREATE TABLE IF NOT EXISTS child_orders (
  child_order_id   TEXT PRIMARY KEY,
  hedge_flow_id    TEXT NOT NULL REFERENCES hedgeflows(hedge_flow_id) ON DELETE CASCADE,
  venue_id         TEXT NOT NULL,
  symbol           TEXT NOT NULL,               -- venue-mapped symbol (e.g. XBTUSD on Kraken)
  side             TEXT NOT NULL CHECK (side IN ('BUY', 'SELL')),
  order_type       TEXT NOT NULL CHECK (order_type IN ('MARKET', 'LIMIT', 'POST_ONLY', 'IOC', 'FOK')),
  qty              NUMERIC(38, 18) NOT NULL CHECK (qty > 0),
  price            NUMERIC(38, 18),              -- nullable for MARKET
  tif              TEXT NOT NULL CHECK (tif IN ('GTC', 'IOC', 'FOK')),
  filled_qty       NUMERIC(38, 18) NOT NULL DEFAULT 0 CHECK (filled_qty >= 0),
  avg_price        NUMERIC(38, 18),
  fee              NUMERIC(38, 18) NOT NULL DEFAULT 0,
  fee_currency     TEXT,
  client_order_id  TEXT NOT NULL,
  venue_order_id   TEXT,                         -- assigned by venue after accept
  status           TEXT NOT NULL CHECK (status IN ('PENDING', 'FILLED', 'PARTIALLY_FILLED', 'CANCELLED', 'REJECTED')),
  error_code       TEXT,
  error_message    TEXT,
  created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  CONSTRAINT child_orders_filled_le_qty CHECK (filled_qty <= qty * 1.01)
);

-- Idempotency: prevent retry duplicates per (hedge_flow_id, client_order_id).
CREATE UNIQUE INDEX IF NOT EXISTS child_orders_idem
  ON child_orders (hedge_flow_id, client_order_id);

CREATE INDEX IF NOT EXISTS idx_child_orders_venue ON child_orders (venue_id, status);
CREATE INDEX IF NOT EXISTS idx_child_orders_hedge_flow ON child_orders (hedge_flow_id);
CREATE INDEX IF NOT EXISTS idx_child_orders_venue_order_id
  ON child_orders (venue_id, venue_order_id) WHERE venue_order_id IS NOT NULL;

-- execution_reports_raw: optional landing zone for normalized ExecutionReport
-- events before aggregation into ClickHouse `execution_reports`. Useful for:
-- (a) idempotency check against duplicate Kafka deliveries;
-- (b) debugging ExecutionReport parse issues.
-- Can be skipped if ledger applies events directly from Kafka with in-memory dedup.
CREATE TABLE IF NOT EXISTS execution_reports_raw (
  report_id        TEXT PRIMARY KEY,             -- venue-provided execId or hash
  intent_id        TEXT NOT NULL,
  hedge_flow_id    TEXT NOT NULL,
  child_order_id   TEXT,
  venue_id         TEXT NOT NULL,
  symbol           TEXT NOT NULL,
  side             TEXT NOT NULL CHECK (side IN ('BUY', 'SELL')),
  status           TEXT NOT NULL,
  filled_qty       NUMERIC(38, 18) NOT NULL,
  remaining_qty    NUMERIC(38, 18),
  avg_price        NUMERIC(38, 18),
  fee              NUMERIC(38, 18),
  fee_currency     TEXT,
  slippage_bps     INTEGER,
  reference_mid    NUMERIC(38, 18),
  raw_payload      JSONB,
  applied_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_exec_reports_raw_hedge ON execution_reports_raw (hedge_flow_id);
CREATE INDEX IF NOT EXISTS idx_exec_reports_raw_venue ON execution_reports_raw (venue_id, applied_at);

-- ===========================================================================
-- F-20 Live Venue Simulator (PR-F20-4). Control-plane + sim-book tables.
-- ADRs: ADR-015 (topic isolation), ADR-016 (separate sim-book), ADR-017
-- (VenueSimulator new component).
-- ===========================================================================

-- sim_sessions: one row per active simulation configuration period.
-- Owner: SimSession Manager (cpp/venues). Source of truth for which
-- venues/instruments run in sim mode and with which behaviour models.
-- Model fields are JSONB so the proto messages (LatencyModel/ImpactModel/
-- FeeModel/RejectionModel) can be stored/hot-reloaded without per-field
-- columns. sim_session_id is a genuine UUID (operator-created), unlike
-- hedgeflows.hedge_flow_id which is composite TEXT.
CREATE TABLE IF NOT EXISTS sim_sessions (
  sim_session_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  name                    TEXT NOT NULL,
  routing_mode            TEXT NOT NULL CHECK (routing_mode IN ('SIM_ONLY', 'LIVE_ONLY', 'SHADOW')),
  scope_venues            TEXT[] NOT NULL DEFAULT '{}',
  scope_instruments       TEXT[] NOT NULL DEFAULT '{}',
  latency_model           JSONB NOT NULL DEFAULT '{}'::jsonb,
  impact_model            JSONB NOT NULL DEFAULT '{}'::jsonb,
  fee_model               JSONB NOT NULL DEFAULT '{}'::jsonb,
  rejection_model         JSONB NOT NULL DEFAULT '{}'::jsonb,
  stale_lob_threshold_ms  INTEGER NOT NULL DEFAULT 2000 CHECK (stale_lob_threshold_ms > 0),
  partial_fill_mode       TEXT NOT NULL DEFAULT 'LEVEL_BY_LEVEL'
                          CHECK (partial_fill_mode IN ('PROPORTIONAL', 'LEVEL_BY_LEVEL', 'NONE')),
  status                  TEXT NOT NULL DEFAULT 'ACTIVE'
                          CHECK (status IN ('ACTIVE', 'PAUSED', 'COMPLETED', 'CANCELLED')),
  created_at              TIMESTAMPTZ NOT NULL DEFAULT now(),
  activated_at            TIMESTAMPTZ,
  completed_at            TIMESTAMPTZ,
  created_by              TEXT NOT NULL DEFAULT 'operator'
);

-- Only one ACTIVE session may scope a given (venue, instrument) at a time —
-- enforced in application logic (overlapping array membership is awkward to
-- express as a PG constraint). This index speeds the lookup.
CREATE INDEX IF NOT EXISTS idx_sim_sessions_status ON sim_sessions (status, created_at DESC);

-- sim_positions: ADR-016 isolated sim-book. The sim-book ledger consumer
-- (subscribed to sim.execution.venue, ADR-015) updates these. NEVER the
-- real `positions` table. Keyed by (sim_session_id, provider, instrument)
-- so sessions are independent and teardown is a single DELETE by session.
CREATE TABLE IF NOT EXISTS sim_positions (
  sim_session_id      UUID NOT NULL REFERENCES sim_sessions(sim_session_id) ON DELETE CASCADE,
  provider_id         TEXT NOT NULL,
  instrument_symbol   TEXT NOT NULL,
  net_qty             NUMERIC(38, 18) NOT NULL DEFAULT 0,
  avg_entry_price     NUMERIC(38, 18) NOT NULL DEFAULT 0,
  realised_pnl        NUMERIC(38, 18) NOT NULL DEFAULT 0,
  updated_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (sim_session_id, provider_id, instrument_symbol)
);

-- sim_hedge_pnl: ADR-016 sim-book aggregate (sim analog of the PG
-- hedgeflows.hedge_pnl/tot_fee sink). Per (session, venue, instrument).
CREATE TABLE IF NOT EXISTS sim_hedge_pnl (
  sim_session_id      UUID NOT NULL REFERENCES sim_sessions(sim_session_id) ON DELETE CASCADE,
  venue_id            TEXT NOT NULL,
  instrument_symbol   TEXT NOT NULL,
  total_hedge_pnl     NUMERIC(38, 18) NOT NULL DEFAULT 0,
  total_fee           NUMERIC(38, 18) NOT NULL DEFAULT 0,
  total_filled_qty    NUMERIC(38, 18) NOT NULL DEFAULT 0,
  trade_count         BIGINT NOT NULL DEFAULT 0,
  updated_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (sim_session_id, venue_id, instrument_symbol)
);
