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
