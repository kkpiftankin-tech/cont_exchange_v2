BEGIN;

-- F15-DATA-1
-- Canonical PostgreSQL schema for replay session lifecycle and summary storage.
-- session_id is indexed by the PRIMARY KEY on replay_sessions.

CREATE TABLE IF NOT EXISTS replay_sessions (
  session_id UUID PRIMARY KEY,
  user_id TEXT NOT NULL,
  name VARCHAR(255) NOT NULL,
  strategy JSONB NOT NULL,
  date_range_from TIMESTAMPTZ NOT NULL,
  date_range_to TIMESTAMPTZ NOT NULL,
  solver_config_id TEXT NOT NULL,
  risk_limits_id TEXT NOT NULL,
  fee_model JSONB NOT NULL,
  session_config_snapshot JSONB,
  status TEXT NOT NULL CHECK (status IN ('pending', 'running', 'completed', 'failed', 'cancelled')),
  total_batches INTEGER,
  progress_batches INTEGER NOT NULL DEFAULT 0,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  started_at TIMESTAMPTZ,
  completed_at TIMESTAMPTZ,
  error_details TEXT,
  CHECK (date_range_from <= date_range_to),
  CHECK (progress_batches >= 0),
  CHECK (total_batches IS NULL OR total_batches >= progress_batches)
);

CREATE INDEX IF NOT EXISTS replay_sessions_user_id_idx
  ON replay_sessions (user_id);

CREATE INDEX IF NOT EXISTS replay_sessions_status_idx
  ON replay_sessions (status);

CREATE INDEX IF NOT EXISTS replay_sessions_created_idx
  ON replay_sessions (created_at DESC);

CREATE INDEX IF NOT EXISTS replay_sessions_user_created_idx
  ON replay_sessions (user_id, created_at DESC);

CREATE INDEX IF NOT EXISTS replay_sessions_status_created_idx
  ON replay_sessions (status, created_at DESC);

CREATE INDEX IF NOT EXISTS replay_sessions_date_range_idx
  ON replay_sessions (date_range_from, date_range_to);

CREATE TABLE IF NOT EXISTS replay_summaries (
  summary_id UUID PRIMARY KEY,
  session_id UUID NOT NULL UNIQUE REFERENCES replay_sessions(session_id) ON DELETE CASCADE,
  total_pnl NUMERIC(24, 8),
  avg_is NUMERIC(24, 8),
  avg_pnl NUMERIC(24, 8),
  std_pnl NUMERIC(24, 8),
  sharpe_ratio NUMERIC(24, 8),
  fill_rate NUMERIC(12, 8),
  avg_vwap NUMERIC(24, 8),
  avg_solve_time_ms NUMERIC(24, 8),
  max_drawdown NUMERIC(24, 8),
  total_batches INTEGER,
  processed_batches INTEGER NOT NULL DEFAULT 0,
  failed_batches INTEGER NOT NULL DEFAULT 0,
  total_fill_events INTEGER,
  partial BOOLEAN NOT NULL DEFAULT FALSE,
  avgis_rule TEXT NOT NULL DEFAULT 'volume_weighted',
  decision_price_source TEXT NOT NULL DEFAULT 'marketdata_mid_with_clearprice_fallback',
  no_requested_volume BOOLEAN NOT NULL DEFAULT FALSE,
  metrics JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  CHECK (processed_batches >= 0),
  CHECK (failed_batches >= 0),
  CHECK (total_batches IS NULL OR total_batches >= processed_batches)
);

CREATE INDEX IF NOT EXISTS replay_summaries_created_at_idx
  ON replay_summaries (created_at DESC);

-- Cache for GET /replay/compare style responses.
CREATE TABLE IF NOT EXISTS replay_compare_cache (
  compare_key TEXT PRIMARY KEY,
  session_a_id UUID NOT NULL REFERENCES replay_sessions(session_id) ON DELETE CASCADE,
  session_b_id UUID NOT NULL REFERENCES replay_sessions(session_id) ON DELETE CASCADE,
  comparison_json JSONB NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  expires_at TIMESTAMPTZ,
  CHECK (session_a_id <> session_b_id)
);

CREATE INDEX IF NOT EXISTS replay_compare_cache_session_pair_idx
  ON replay_compare_cache (session_a_id, session_b_id);

-- Storage for single-batch audit-mode replay runs and their diffs.
CREATE TABLE IF NOT EXISTS replay_audit_runs (
  audit_run_id UUID PRIMARY KEY,
  session_id UUID REFERENCES replay_sessions(session_id) ON DELETE SET NULL,
  requested_by TEXT NOT NULL,
  batch_id TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('pending', 'running', 'completed', 'failed', 'cancelled')),
  equivalent BOOLEAN,
  tolerance_json JSONB,
  replay_result_json JSONB,
  production_result_json JSONB,
  diff_json JSONB,
  error_details TEXT,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  completed_at TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS replay_audit_runs_session_id_idx
  ON replay_audit_runs (session_id);

CREATE INDEX IF NOT EXISTS replay_audit_runs_created_at_idx
  ON replay_audit_runs (created_at DESC);

COMMIT;
