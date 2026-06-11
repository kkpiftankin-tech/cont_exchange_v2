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

-- ===========================================================================
-- F-15 Backtest / Replay: config registries (solver / risk / fee / reward).
-- Schema mirrors PostgresReplayConfigRepository::EnsureSchema (id, version,
-- body_json) so these CREATE TABLE IF NOT EXISTS are no-ops when the backtest
-- service has already created them at runtime. Seeded with the production
-- config ids referenced by the BacktestReplay create form defaults
-- (solver-prod-v4, risk-standard) so the page works out of the box.
-- See docs/06-api/rest/replay.md and feature.yaml F-15.
-- ===========================================================================
CREATE TABLE IF NOT EXISTS replay_solver_configs (
  id         TEXT PRIMARY KEY,
  version    INTEGER NOT NULL DEFAULT 0,
  body_json  TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS replay_risk_limits (
  id         TEXT PRIMARY KEY,
  version    INTEGER NOT NULL DEFAULT 0,
  body_json  TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS replay_fee_models (
  id         TEXT PRIMARY KEY,
  version    INTEGER NOT NULL DEFAULT 0,
  body_json  TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS replay_reward_configs (
  id         TEXT PRIMARY KEY,
  version    INTEGER NOT NULL DEFAULT 0,
  body_json  TEXT NOT NULL
);

INSERT INTO replay_solver_configs (id, version, body_json) VALUES (
  'solver-prod-v4', 1,
  '{"batchintervalms":1000,"maxiterations":128,"tolerance":0.0001,"epsilonliquidity":0.00001,"feemodel":{"makerfeerate":0.0002,"takerfeerate":0.0005}}'
) ON CONFLICT (id) DO NOTHING;

INSERT INTO replay_risk_limits (id, version, body_json) VALUES (
  'risk-standard', 1,
  '{"maxnotional":1000000,"maxposition":100,"maxleverage":3,"maxorderrate":20,"whitelist":["BTCUSDT"]}'
) ON CONFLICT (id) DO NOTHING;

-- Fee model id 'production' is the id the create form sends as feemodel
-- {"production": true}; the config resolver looks it up by id="production".
INSERT INTO replay_fee_models (id, version, body_json) VALUES (
  'production', 1,
  '{"makerfeerate":0.0002,"takerfeerate":0.0005}'
) ON CONFLICT (id) DO NOTHING;

-- ===========================================================================
-- F-09: Batch/Combo/Multi-leg Orders (ADR-032, ADR-033)
-- Sources: IN-011 §8, §14.1, §15; docs/07-data/oltp-schema.md §F-09.
-- Owner-services: order_flow (create/normalize), matching (execution_groups,
--   group_state_transitions).
-- Creation order respects FK dependencies:
--   batch_orders → combo_orders → combo_order_legs / combo_constraints /
--   conditional_links → execution_groups → group_state_transitions.
-- All monetary / qty / price fields: NUMERIC(38,18) per CLAUDE.md §9.
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- batch_orders: top-level client parent object grouping multiple ComboOrders,
-- conditional branches, or plain FlowOrders into a single submission.
-- NOTE: "batch" here is the client-facing order type (F-09), NOT the
--       matching-cycle batch (F-04 BatchResult).
-- Writers: order_flow (create/cancel), matching (status transitions).
-- Readers: order_flow, matching, risk, gateway.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS batch_orders (
  batch_order_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id             TEXT NOT NULL,
  account_id          TEXT NOT NULL,
  -- order_type: batch|combo|basket|spread|conditional|oco|bracket
  order_type          TEXT NOT NULL,
  -- execution_mode: orchestration_only|multileg_vector_solver
  execution_mode      TEXT NOT NULL,
  -- status: §15.1 lifecycle
  -- draft/risk_pending/active/waiting_for_trigger/partially_filled/filled/
  -- cancelled/expired/degraded/rollback_pending/rolledback/rejected
  status              TEXT NOT NULL,
  time_window_start   TIMESTAMPTZ,
  time_window_end     TIMESTAMPTZ,
  created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

  CONSTRAINT batch_orders_window_valid
    CHECK (time_window_end IS NULL OR time_window_end > time_window_start)
);

CREATE INDEX IF NOT EXISTS idx_batch_orders_user_id
  ON batch_orders (user_id);
CREATE INDEX IF NOT EXISTS idx_batch_orders_active_status
  ON batch_orders (status, created_at DESC)
  WHERE status IN ('active', 'partially_filled', 'waiting_for_trigger', 'risk_pending');

-- ---------------------------------------------------------------------------
-- combo_orders: a multi-leg order with shared executionMode, atomicityPolicy,
-- and combo-specific policies. May be nested under a batch_order or
-- autonomous (batch_order_id IS NULL).
-- Writers: order_flow, matching.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS combo_orders (
  combo_order_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  -- NULL = autonomous combo order not wrapped in a batch_order
  batch_order_id          UUID REFERENCES batch_orders(batch_order_id) ON DELETE CASCADE,
  -- T-F09-062: owner — needed by matching loader → ExecutionGroup.user_id →
  -- ledger grouped postings + hierarchical PnL.
  user_id                 TEXT,
  account_id              TEXT,
  -- combo_type: pair/basket/spread/conditional/oco/bracket/factor/budget
  combo_type              TEXT NOT NULL,
  -- execution_mode: orchestration_only|multileg_vector_solver
  execution_mode          TEXT NOT NULL,
  -- status: §15.1 lifecycle
  status                  TEXT NOT NULL,
  -- ratio_basis: notional_weight|quantity_ratio|NULL
  ratio_basis             TEXT,
  -- atomicity_policy: strict_atomic|scalable_atomic|best_effort|
  --                   sequential_fallback|external_compensating
  atomicity_policy        TEXT NOT NULL,
  -- atomicity_scope: internal_batch|venue_native|external_compensating|none
  atomicity_scope         TEXT NOT NULL,
  -- fallback_policy: scale_down|wait_next_batch|cancel|degrade|compensate
  fallback_policy         TEXT NOT NULL,
  -- min_execution_scale: α ∈ [0,1]; NULL = no minimum
  min_execution_scale     NUMERIC(38,18),
  -- max_ratio_deviation_bps: NULL = no cap
  max_ratio_deviation_bps INTEGER,
  created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),

  CONSTRAINT combo_orders_execution_scale_range
    CHECK (min_execution_scale IS NULL
           OR (min_execution_scale >= 0 AND min_execution_scale <= 1))
);

CREATE INDEX IF NOT EXISTS idx_combo_orders_batch_order_id
  ON combo_orders (batch_order_id)
  WHERE batch_order_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_combo_orders_active_status
  ON combo_orders (status, updated_at DESC)
  WHERE status IN ('active', 'partially_filled', 'waiting_for_trigger', 'risk_pending');

-- ---------------------------------------------------------------------------
-- combo_order_legs: one row per leg of a ComboOrder.
-- SEPARATE table from flow_order_legs (ADR-032 recommendation / oltp-schema.md §F-09):
--   - flow_order_legs PK is (order_id, instrument_symbol) with only `weight`;
--     adding NOT NULL CSLO fields would break F-02 and PostgresFlowOrderRepository.
--   - In multileg_vector_solver mode, legs are NOT standalone FlowOrders;
--     FK to flow_orders would be semantically wrong.
--   - In orchestration_only mode, each leg creates a FlowOrder naturally via
--     flow_order_legs; combo_order_legs stores the original intent for
--     parent-level reporting.
-- Writers: order_flow (insert), matching (filled_cum, status updates).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS combo_order_legs (
  leg_id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  parent_order_id     UUID NOT NULL REFERENCES combo_orders(combo_order_id) ON DELETE CASCADE,
  instrument_symbol   TEXT NOT NULL,
  side                TEXT NOT NULL CHECK (side IN ('buy', 'sell')),
  -- ratio XOR weight: exactly one must be NOT NULL (enforced by CHECK below)
  ratio               NUMERIC(38,18),
  weight              NUMERIC(38,18),
  -- ratio_basis overrides the parent combo_order ratio_basis for this leg
  ratio_basis         TEXT,
  -- CSLO price range
  p_low               NUMERIC(38,18) NOT NULL,
  p_high              NUMERIC(38,18) NOT NULL,
  -- CSLO speed and max volume
  q_rate              NUMERIC(38,18) NOT NULL,
  q_max               NUMERIC(38,18) NOT NULL,
  filled_cum          NUMERIC(38,18) NOT NULL DEFAULT 0,
  venue_preferences   TEXT[],
  -- status: §15.2 leg lifecycle
  -- inactive/active/waiting_for_trigger/partially_filled/filled/cancelled/
  -- blocked_by_group/blocked_by_atomicity/failed_external/compensated
  status              TEXT NOT NULL DEFAULT 'inactive',

  CONSTRAINT combo_order_legs_price_range
    CHECK (p_low > 0 AND p_high >= p_low),
  CONSTRAINT combo_order_legs_q_rate_positive
    CHECK (q_rate > 0),
  CONSTRAINT combo_order_legs_q_max_positive
    CHECK (q_max > 0),
  CONSTRAINT combo_order_legs_filled_cum_valid
    CHECK (filled_cum >= 0 AND filled_cum <= q_max),
  -- Exactly one of ratio / weight must be set (XOR)
  CONSTRAINT combo_order_legs_ratio_xor_weight
    CHECK ((ratio IS NOT NULL) <> (weight IS NOT NULL))
);

CREATE INDEX IF NOT EXISTS idx_combo_order_legs_parent_order
  ON combo_order_legs (parent_order_id);
CREATE INDEX IF NOT EXISTS idx_combo_order_legs_symbol
  ON combo_order_legs (instrument_symbol);
CREATE INDEX IF NOT EXISTS idx_combo_order_legs_active_status
  ON combo_order_legs (status)
  WHERE status IN ('active', 'partially_filled', 'waiting_for_trigger');

-- ---------------------------------------------------------------------------
-- combo_constraints: shared constraints on a ComboOrder
-- (ratio/spread/budget/factor/margin/risk).
-- Writers: order_flow.
-- Readers: matching (enforcement), risk.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS combo_constraints (
  constraint_id   UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  parent_order_id UUID NOT NULL REFERENCES combo_orders(combo_order_id) ON DELETE CASCADE,
  -- constraint_type: max_weight_deviation|max_total_notional|spread_range|
  --   factor_neutrality|max_leverage|max_margin|risk_limit|ratio_equality
  constraint_type TEXT NOT NULL,
  -- coefficients: {symbol: coeff} JSON for spread/factor constraints
  coefficients    JSONB,
  lower_bound     NUMERIC(38,18),
  upper_bound     NUMERIC(38,18),
  value           NUMERIC(38,18),
  value_bps       INTEGER,
  -- severity: hard (reject if violated) | soft (warn / degrade)
  severity        TEXT NOT NULL DEFAULT 'hard' CHECK (severity IN ('hard', 'soft'))
);

CREATE INDEX IF NOT EXISTS idx_combo_constraints_parent_order
  ON combo_constraints (parent_order_id);

-- ---------------------------------------------------------------------------
-- conditional_links: edges of the activation/cancellation graph
-- (OCO / bracket / conditional order types).
-- Writers: order_flow.
-- Readers: matching (trigger evaluation), order_flow (status propagation).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS conditional_links (
  link_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  parent_order_id UUID NOT NULL REFERENCES combo_orders(combo_order_id) ON DELETE CASCADE,
  -- from_leg_id / to_leg_id reference legs in the same ComboOrder
  from_leg_id     UUID NOT NULL REFERENCES combo_order_legs(leg_id) ON DELETE CASCADE,
  to_leg_id       UUID NOT NULL REFERENCES combo_order_legs(leg_id) ON DELETE CASCADE,
  -- link_type: oco|bracket|conditional
  link_type       TEXT NOT NULL,
  -- condition: trigger expression; NULL = unconditional OCO cancellation
  condition       JSONB,

  CONSTRAINT conditional_links_no_self_loop
    CHECK (from_leg_id <> to_leg_id)
);

CREATE INDEX IF NOT EXISTS idx_conditional_links_parent_order
  ON conditional_links (parent_order_id);
CREATE INDEX IF NOT EXISTS idx_conditional_links_from_leg
  ON conditional_links (from_leg_id);

-- F-09 MVP-5 (ADR-037): требования компенсации при сбое внешней ноги combo.
-- external_compensating: внутренние ноги исполнены, внешняя провалилась → pending.
-- Сам компенсирующий трейд (реверс/повторный хедж) — operator/policy-driven (MVP-6).
CREATE TABLE IF NOT EXISTS combo_compensations (
  compensation_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  parent_order_id     UUID NOT NULL REFERENCES combo_orders(combo_order_id) ON DELETE CASCADE,
  leg_id              UUID NOT NULL,
  -- report_id внешней ExecutionReport, вызвавшей компенсацию (idempotency)
  report_id           TEXT NOT NULL,
  -- reason: rejected | timeout | cancelled (статус провала внешней ноги)
  reason              TEXT NOT NULL,
  internal_filled_qty NUMERIC(38,18),
  status              TEXT NOT NULL DEFAULT 'pending'
                        CHECK (status IN ('pending', 'resolved', 'cancelled')),
  -- MVP-6 (ADR-039): operator-driven resolution (audit). NULL пока pending.
  -- resolution_action: reverse_internal | retry_external | accept
  resolution_action   TEXT,
  operator_id         TEXT,
  resolving_ref       TEXT,  -- id реверсивной FlowOrder / retry-intent
  resolved_at         TIMESTAMPTZ,
  created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

  CONSTRAINT combo_compensations_idem UNIQUE (parent_order_id, leg_id, report_id)
);

CREATE INDEX IF NOT EXISTS idx_combo_compensations_parent
  ON combo_compensations (parent_order_id);
CREATE INDEX IF NOT EXISTS idx_combo_compensations_status
  ON combo_compensations (status);

-- ---------------------------------------------------------------------------
-- execution_groups: result of one grouped solve cycle for one ComboOrder.
-- execution_group_id is the idempotency key for ledger (ADR-033).
-- Writer: matching ONLY.
-- Readers: ledger (idempotent apply by execution_group_id), order_flow,
--          risk, observability, backtest.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS execution_groups (
  execution_group_id  UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  batch_id            TEXT NOT NULL,
  parent_order_id     UUID NOT NULL REFERENCES combo_orders(combo_order_id),
  execution_mode      TEXT NOT NULL,
  -- group_status: §15.3 lifecycle
  -- filled/partial/waiting_next_batch/cancelled_by_atomicity/degraded/
  -- compensating/rollback_pending/rolledback/failed
  group_status        TEXT NOT NULL,
  -- execution_scale: α ∈ [0,1], actual fill fraction of max_qty
  execution_scale     NUMERIC(38,18) NOT NULL,
  atomicity_policy    TEXT NOT NULL,
  atomicity_scope     TEXT NOT NULL,
  -- fallback_action: NULL if no fallback was applied in this cycle
  fallback_action     TEXT,
  -- ratio_deviation_bps: actual deviation from target ratios; NULL if not applicable
  ratio_deviation_bps INTEGER,
  -- leg_results: [{legId, execQty, execPrice, fillId}]
  leg_results         JSONB,
  -- violated_constraints: [constraint_id, ...]
  violated_constraints JSONB,
  -- solver_diagnostics: {groupSolveTimeMs, bindingLegs[], bindingConstraints[]}
  solver_diagnostics  JSONB,
  created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

  CONSTRAINT execution_groups_scale_range
    CHECK (execution_scale >= 0 AND execution_scale <= 1)
);

CREATE INDEX IF NOT EXISTS idx_execution_groups_parent_order
  ON execution_groups (parent_order_id);
CREATE INDEX IF NOT EXISTS idx_execution_groups_batch_id
  ON execution_groups (batch_id);

-- ---------------------------------------------------------------------------
-- group_state_transitions: audit log of status transitions + idempotency
-- deduplication for repeated Kafka deliveries (ADR-032, ADR-020).
-- Writer: matching (via ledger / order_flow on status application).
-- idempotency_key: UNIQUE constraint prevents duplicate state application.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS group_state_transitions (
  id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  group_id         UUID NOT NULL REFERENCES execution_groups(execution_group_id) ON DELETE CASCADE,
  from_status      TEXT,
  to_status        TEXT NOT NULL,
  -- batch_id: NULL for manual / operator-initiated transitions
  batch_id         TEXT,
  -- reason: machine-readable transition code
  reason           TEXT,
  -- idempotency_key: dedup token for repeated Kafka at-least-once deliveries
  idempotency_key  TEXT NOT NULL,
  created_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),

  CONSTRAINT group_state_transitions_idem_unique
    UNIQUE (idempotency_key)
);

CREATE INDEX IF NOT EXISTS idx_group_state_transitions_group_id
  ON group_state_transitions (group_id);
CREATE INDEX IF NOT EXISTS idx_group_state_transitions_created_at
  ON group_state_transitions (created_at DESC);
