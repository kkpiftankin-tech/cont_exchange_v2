---
id: DOC-DATA-HEDGEFLOWS
phase: 07-data
status: schema-ready-impl-pending
owner: core-team
source:
  - IN-005 §1 «HedgeFlow (PostgreSQL hedgeflows)»
  - IN-008 v2 (PR-F12-2 schema applied 2026-05-23)
related:
  - docs/07-data/child-orders.md
  - docs/07-data/execution-reports.md
  - docs/02-system/features/F-12-execution-hedge/
  - contracts/proto/fob/hedge/v1/hedge.proto
---

# PostgreSQL Table: `hedgeflows`

> **Status:** ✅ DDL добавлена в [`infra/postgres/init.sql`](../../infra/postgres/init.sql) (PR-F12-2, 2026-05-23). Поддерживается на запуске PostgreSQL контейнера в dev/staging. C++ репозиторий-код (`cpp/venues/src/infra/postgres/hedgeflow_repository.*`) ещё не реализован — таблица будет пустой до PR-F12-3.

## Owner

[venue-execution-adapter](../05-components/venue-execution-adapter/overview.md) — единственный writer. Read access: REST gateway (GET `/api/v1/hedge/flows`), backtest-replay.

## Purpose

Источник истины для сессии хеджа одного `ExecutionIntent`. Один HedgeFlow содержит:
- параметры цели (`target_qty`, `target_notional`, `reference_mid`, `timeout_ms`);
- агрегированные результаты (`filled_qty`, `avg_fill_price`, `tot_fee`, `hedge_pnl`);
- статус (`OPEN` → terminal).

Связи:
- 1:N с [`child_orders`](child-orders.md) — все child orders по этому HedgeFlow.
- N:1 с `execution_reports` (ClickHouse) — каждый ExecutionReport содержит `hedge_flow_id`.

## DDL (target)

```sql
-- F-12 Execution Hedge: hedgeflows table.
--
-- Lifecycle: OPEN -> COMPLETED | UNDERFILLED | REJECTED | RISK_REJECTED | CANCELLED
--
-- Statuses соответствуют fob.hedge.v1.HedgeFlowStatus.

-- =====================================================================
-- ACTUAL SCHEMA (источник истины: infra/postgres/init.sql:123-158).
-- Различия с исходной IN-005 §1:
--   1. id-колонки — TEXT, не UUID. Причина: matching's F-04 external_fill
--      path использует composite intent_id формата
--      "<batch>|<order>|<symbol>|<venue>|external_fill_N", который не
--      является UUID. Real F-12 hedge intents используют UUID-формат
--      "<batch>|hedge|<provider>|<symbol>", но обе формы должны лежать
--      в одной таблице — поэтому TEXT.
--   2. Decimal precision NUMERIC(38,18), не (24,8) — согласовано с
--      flow_orders для consistency и финансовой точности.
--   3. Колонка `source` (IN-005 §1) НЕ присутствует в init.sql. В коде
--      ExecutionIntent.source существует (proto), но venues currently
--      не персистит его в PG. Open: добавить колонку в follow-up PR
--      или вычислять origin из intent_id pattern в UI (то, что
--      делает PR-F12-9c manual-overrides endpoint).
--   4. Дополнительная семантика timeout_ms — у matching default 30000
--      (HEDGE_INTENT_TIMEOUT_MS env, PR-F12-5), не 5000.
-- =====================================================================
CREATE TABLE IF NOT EXISTS hedgeflows (
    hedge_flow_id    TEXT PRIMARY KEY,                -- (was UUID in IN-005 §1)
    intent_id        TEXT NOT NULL,                   -- (was UUID)
    batch_id         TEXT,                            -- back-ref в F-04 BatchResult; nullable: manual override has no batch
    provider_id      TEXT NOT NULL,
    symbol           TEXT NOT NULL,
    side             TEXT NOT NULL CHECK (side IN ('BUY', 'SELL')),
    -- (no `source` column — derived from intent_id pattern in UI)

    -- target
    target_qty       NUMERIC(38,18) NOT NULL CHECK (target_qty > 0),  -- was (24,8)
    target_notional  NUMERIC(38,18),                                  -- now nullable; was NOT NULL (24,8)
    reference_mid    NUMERIC(38,18),                                  -- was (24,8)

    urgency          TEXT NOT NULL CHECK (urgency IN ('LOW', 'MEDIUM', 'HIGH')),
    timeout_ms       INTEGER NOT NULL CHECK (timeout_ms > 0),         -- default in matching: 30000

    -- aggregates (обновляются при каждом ExecutionReport)
    filled_qty       NUMERIC(38,18) NOT NULL DEFAULT 0 CHECK (filled_qty >= 0),
    avg_fill_price   NUMERIC(38,18),
    tot_fee          NUMERIC(38,18) NOT NULL DEFAULT 0,
    hedge_pnl        NUMERIC(38,18),                                  -- computed by Settlement Ledger (PR-F12-3c)

    -- lifecycle
    status           TEXT NOT NULL CHECK (status IN ('OPEN','COMPLETED','UNDERFILLED','REJECTED','RISK_REJECTED','CANCELLED')),
    error_code       TEXT,
    error_message    TEXT,

    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    completed_at     TIMESTAMPTZ,

    -- consistency: 1% overfill tolerance (matches IN-008 §1)
    CONSTRAINT hedgeflows_filled_le_target CHECK (filled_qty <= target_qty * 1.01)
);

-- (note: the IN-005 draft included a `hedgeflows_terminal_completed_at`
--  constraint and a `hedgeflows_intent_id_uniq` unique index. Neither
--  is present in init.sql today — intent_id can repeat in dev when
--  matching's external_fill path emits multiple intents with the same
--  composite key across batches. If this becomes a real conflict in
--  prod we'll either dedupe earlier in venues or relax the constraint
--  with a (intent_id, batch_id) compound. Open question.)

CREATE INDEX IF NOT EXISTS idx_hedgeflows_provider_symbol ON hedgeflows (provider_id, symbol);
CREATE INDEX IF NOT EXISTS idx_hedgeflows_batch_id ON hedgeflows (batch_id) WHERE batch_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_hedgeflows_status_open ON hedgeflows (status, updated_at) WHERE status IN ('OPEN');
CREATE INDEX IF NOT EXISTS idx_hedgeflows_status_underfilled ON hedgeflows (status, created_at) WHERE status IN ('UNDERFILLED', 'REJECTED');
```

## Поля

| Поле | Тип | Описание | Источник |
| --- | --- | --- | --- |
| `hedge_flow_id` | TEXT PK | сессия хеджа; для F-12 auto-hedge — `<batch>\|hedge\|<provider>\|<symbol>`; для F-04 external_fill — `<batch>\|<order>\|<symbol>\|<venue>\|external_fill_N` | matching builder |
| `intent_id` | TEXT | ID `ExecutionIntent`; для F-12 — `<hedge_flow_id>\|intent`, для F-04 — composite (тот же что hedge_flow_id) | из Kafka |
| `batch_id` | TEXT (nullable) | для AUTO_BATCH — связь с F-04; NULL при manual override | из ExecutionIntent |
| `provider_id` | TEXT | user_id владельца хеджа | из intent |
| `symbol` | TEXT | internal instrument symbol (BTC/USDT) | |
| `side` | enum | BUY/SELL | |
| ~~`source`~~ | — | НЕ персистится в PG; см. drift-note. Origin выводится из intent_id pattern на UI (PR-F12-9c `manual-overrides` endpoint). | proto only |
| `target_qty` | NUMERIC(38,18) | base units | |
| `target_notional` | NUMERIC(38,18) (nullable) | quote units; computed by matching = target_qty * reference_mid | |
| `reference_mid` | NUMERIC(38,18) (nullable) | clearing price (F-04 или snapshot.clearing_price) | matching's BuildFromHedgeTriggerDecisions |
| `timeout_ms` | INTEGER | default 30000 (HEDGE_INTENT_TIMEOUT_MS env, PR-F12-5) | matching |
| ~~`max_slippage_bps`~~ | — | колонка отсутствует в init.sql; есть в proto, но не персистится. | proto only |
| `urgency` | enum | LOW/MEDIUM/HIGH (HEDGE_INTENT_URGENCY env) | matching |
| `filled_qty` | NUMERIC(38,18) | агрегат, обновляется venues' `PostgresHedgeflowRepository::ApplyReport` | venues_loop.cpp |
| `avg_fill_price` | NUMERIC(38,18) (nullable) | weighted avg обновляется при ApplyReport | venues |
| `tot_fee` | NUMERIC(38,18) | sum of fees; обновляется ledger's PostgresHedgeflowPnlSink (PR-F12-3c) | ledger_uc.cpp |
| `hedge_pnl` | NUMERIC(38,18) (nullable) | вычисляется ledger: (executed_price - internal_price) * qty с учётом side; в dev может быть 0 из-за venues-sim-zero-execution-price knownIssue | ledger_uc.cpp (PR-F12-3c) |
| `status` | enum | lifecycle, см. CHECK constraint | venues' ApplyReport |
| `error_code`, `error_message` | TEXT | при non-success terminal status | venues / ledger |
| `created_at`, `updated_at`, `completed_at` | TIMESTAMPTZ | venues sets updated_at + completed_at on terminal | |

## Retention

- Active rows (status=OPEN): без retention (всё время open).
- Terminal rows: 365 дней в OLTP; long-term archive в ClickHouse `execution_reports` (90+ дней) + S3 long-term (за пределами scope).

## Backup

- pg_dump nightly.
- WAL streaming для PITR.

## Consistency / Transactionality

- INSERT hedgeflows + первый INSERT child_orders в одной транзакции (atomic creation).
- UPDATE hedgeflows.filled_qty/avg_fill_price/status выполняется в транзакции с UPDATE child_orders.

## Migration

В follow-up PR `feat/f12-postgres`:
1. добавить DDL в [`infra/postgres/init.sql`](../../infra/postgres/init.sql);
2. написать `cpp/venues/src/infra/postgres_hedgeflow_repository.{hpp,cpp}`;
3. wire в `ExecuteOnVenue` / `ExecutionIntentsConsumer`;
4. integration test creating и updating HedgeFlow.

## Used In Features

- [F-12. Execution Hedge](../02-system/features/F-12-execution-hedge/) — primary.
- [F-13. Post-Trade Report](../02-system/features/F-13-posttrade-report/) — join по `hedge_flow_id`.
- [F-15. Backtest / Replay](../02-system/features/F-15-backtest-replay/).
- [F-16. Operator Console](../02-system/features/F-16-operator-panel-and-kill-switch/) — Reconciliation Alerts.
- [F-17. Monitoring](../02-system/features/F-17-monitoring-and-alerts/) — fill rate, latency metrics.

## Source Fragments

- IN-005 §1 «HedgeFlow (PostgreSQL hedgeflows)»
- `contracts/proto/fob/hedge/v1/hedge.proto` → `HedgeFlow`
