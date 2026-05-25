---
id: DOC-DATA-EXECUTION-REPORTS
phase: 07-data
status: schema-ready-impl-pending
owner: core-team
source:
  - IN-005 §1 «ExecutionReports (ClickHouse execution_reports)»
  - IN-008 v2 (PR-F12-2 schema applied 2026-05-23)
related:
  - docs/07-data/hedgeflows.md
  - docs/07-data/child-orders.md
  - docs/02-system/features/F-12-execution-hedge/
  - docs/06-api/messaging/execution-venue.md
  - contracts/proto/fob/execution/v1/execution.proto
---

# ClickHouse Table: `execution_reports`

> **Status:** ✅ DDL добавлена в [`infra/clickhouse/init.sql`](../../infra/clickhouse/init.sql) (PR-F12-2, 2026-05-23) с partition `toYYYYMM(event_time_ms)`, 90-day TTL и projection `prj_by_hedge_flow` для drill-down. Kafka consumer (`execution.venue` → CH ingestion) ещё не реализован в `cpp/market_data` — таблица будет наполняться после PR-F12-3.

## Owner

- **Writer:** [venue-execution-adapter](../05-components/venue-execution-adapter/overview.md) — через Kafka `execution.venue` → ClickHouse Kafka engine table + materialized view → MergeTree.
- **Readers:** [observability-reporting](../05-components/observability-reporting/overview.md), [backtest-replay](../05-components/backtest-replay/overview.md), F-13 post-trade reports, F-17 alerts.

## Purpose

История всех ExecutionReport для:

- post-trade analytics (slippage, fill rate, hedge PnL, fee analysis);
- replay / backtest (F-15);
- audit trail (compliance, regulatory exports);
- ML / agent logs (F-12 как state-action log для будущих policies).

Retention: **90+ дней** (per IN-005 §11 DoD).

## DDL (target)

```sql
-- F-12 Execution Hedge: ClickHouse execution_reports table.
--
-- Schema mirrors fob.execution.v1.ExecutionReport.
-- Ingestion path: Kafka 'execution.venue' (Protobuf) -> Kafka engine table
-- -> materialized view -> MergeTree.

CREATE TABLE IF NOT EXISTS execution_reports (
    -- identification
    report_id        String,
    intent_id        String,
    hedge_flow_id    String,
    child_order_id   String,
    batch_id         String,                       -- back-ref F-04
    provider_id      String,

    -- venue
    venue_id         LowCardinality(String),       -- binance / coinbase / uniswap_v3 / venue_sim
    venue_symbol     String,
    symbol           LowCardinality(String),       -- internal symbol
    side             Enum8('BUY' = 1, 'SELL' = 2),

    -- venue order ids
    venue_order_id   String,
    client_order_id  String,

    -- status (mirrors ExecutionReportStatus)
    status           Enum8(
        'NEW' = 1, 'PARTIALLY_FILLED' = 2, 'FILLED' = 3,
        'CANCELLED' = 4, 'REJECTED' = 5, 'EXPIRED' = 6,
        'OVERFILL_GUARD' = 7, 'UNDERFILLED' = 8
    ),
    reason           String,                       -- свободная форма для REJECTED/CANCELLED

    -- quantities and prices
    filled_qty       Float64,
    remaining_qty    Float64,
    average_price    Float64,
    reference_mid    Float64,
    slippage_bps     Float64,
    hedge_pnl        Float64,

    -- fee
    fee_amount       Float64,
    fee_currency     LowCardinality(String),

    -- urgency (для anti-join с intent)
    urgency          LowCardinality(String),

    -- timing
    timestamp        DateTime64(3, 'UTC'),         -- event_time из ExecutionReport.meta
    intent_created_at DateTime64(3, 'UTC'),        -- для latency analysis
    latency_ms       UInt32 MATERIALIZED toUInt32((timestamp - intent_created_at) * 1000),

    ingested_at      DateTime DEFAULT now(),

    INDEX idx_hedge_flow hedge_flow_id TYPE bloom_filter GRANULARITY 1,
    INDEX idx_intent intent_id TYPE bloom_filter GRANULARITY 1
)
ENGINE = MergeTree()
PARTITION BY toYYYYMM(timestamp)
ORDER BY (symbol, venue_id, timestamp);

-- Kafka source table
CREATE TABLE IF NOT EXISTS execution_reports_kafka (
    -- те же поля, в том же порядке
    report_id String, intent_id String, hedge_flow_id String, child_order_id String,
    batch_id String, provider_id String, venue_id String, venue_symbol String,
    symbol String, side String,
    venue_order_id String, client_order_id String,
    status String, reason String,
    filled_qty Float64, remaining_qty Float64, average_price Float64,
    reference_mid Float64, slippage_bps Float64, hedge_pnl Float64,
    fee_amount Float64, fee_currency String,
    urgency String,
    timestamp DateTime64(3, 'UTC'), intent_created_at DateTime64(3, 'UTC')
) ENGINE = Kafka SETTINGS
    kafka_broker_list = 'redpanda:9092',
    kafka_topic_list = 'execution.venue',
    kafka_group_name = 'ch-execution-reports',
    kafka_format = 'Protobuf',
    kafka_schema = 'fob/execution/v1/execution.proto:ExecutionReport';

-- materialized view: Kafka -> MergeTree
CREATE MATERIALIZED VIEW IF NOT EXISTS execution_reports_mv TO execution_reports AS
SELECT
    report_id, intent_id, hedge_flow_id, child_order_id,
    batch_id, provider_id, venue_id, venue_symbol, symbol,
    side,  -- enum cast выполняется неявно при INSERT
    venue_order_id, client_order_id,
    status, reason,
    filled_qty, remaining_qty, average_price,
    reference_mid, slippage_bps, hedge_pnl,
    fee_amount, fee_currency, urgency,
    timestamp, intent_created_at
FROM execution_reports_kafka;
```

## Поля

| Поле | Тип | Описание |
| --- | --- | --- |
| `report_id` | String | UUID отчёта |
| `intent_id` | String | back-ref к ExecutionIntent |
| `hedge_flow_id` | String | сессия хеджа |
| `child_order_id` | String | child order на venue |
| `batch_id` | String | back-ref к F-04 BatchResult |
| `provider_id` | String | |
| `venue_id`, `venue_symbol`, `symbol`, `side` | string/enum | venue + instrument |
| `venue_order_id`, `client_order_id` | String | ID на стороне venue |
| `status` | enum | ExecutionReportStatus |
| `reason` | String | для REJECTED/CANCELLED/EXPIRED |
| `filled_qty`, `remaining_qty`, `average_price`, `reference_mid`, `slippage_bps`, `hedge_pnl`, `fee_amount`, `fee_currency` | numeric | trade metrics |
| `urgency` | LowCardinality(String) | LOW/MEDIUM/HIGH |
| `timestamp` | DateTime64(3, UTC) | event time |
| `intent_created_at` | DateTime64(3, UTC) | для latency analysis |
| `latency_ms` | UInt32 MATERIALIZED | computed: `(timestamp - intent_created_at) * 1000` |
| `ingested_at` | DateTime | INSERT timestamp |

## Partitioning / Ordering

- `PARTITION BY toYYYYMM(timestamp)` — месячные партиции (типичный размер ~10-100 GB).
- `ORDER BY (symbol, venue_id, timestamp)` — оптимизировано для запросов по symbol/venue в timeline.

## Retention

- Минимум **90 дней** (per IN-005 §11 DoD).
- Drop partition после 365 дней через `ALTER TABLE execution_reports DROP PARTITION ...` cron.

## Query patterns

```sql
-- Fill rate per venue за последние 24h
SELECT
    venue_id,
    countIf(status = 'FILLED') / count() AS fill_rate,
    avg(slippage_bps) AS avg_slippage_bps,
    avg(latency_ms) AS avg_latency_ms
FROM execution_reports
WHERE timestamp >= now() - INTERVAL 24 HOUR
  AND status IN ('FILLED', 'PARTIALLY_FILLED', 'REJECTED', 'EXPIRED')
GROUP BY venue_id
ORDER BY fill_rate DESC;

-- Hedge PnL по symbol за месяц
SELECT
    symbol,
    sum(hedge_pnl) AS total_pnl,
    sum(fee_amount) AS total_fees,
    sum(filled_qty * average_price) AS notional
FROM execution_reports
WHERE timestamp >= toStartOfMonth(now())
  AND status = 'FILLED'
GROUP BY symbol;

-- Timeline одного HedgeFlow
SELECT timestamp, venue_id, status, filled_qty, average_price, slippage_bps, hedge_pnl
FROM execution_reports
WHERE hedge_flow_id = '...'
ORDER BY timestamp ASC;
```

## Idempotency

- `(report_id, hedge_flow_id, child_order_id)` — composite уникальный ключ де-факто.
- В случае at-least-once Kafka может быть дубликат строк; для дедупликации в read-queries использовать `argMax(... , timestamp)` или `LIMIT 1 BY report_id`.
- Альтернатива: `ReplacingMergeTree(ingested_at)` — open question (см. T-F12-203).

## Migration

В follow-up PR `feat/f12-clickhouse`:
1. DDL в [`infra/clickhouse/init.sql`](../../infra/clickhouse/init.sql);
2. integration test: publish 100 ExecutionReport → query count == 100;
3. dedup test: re-publish same events → confirm idempotency strategy (FINAL / ReplacingMergeTree).

## Used In Features

- [F-12. Execution Hedge](../02-system/features/F-12-execution-hedge/) — primary.
- [F-13. Post-Trade Report](../02-system/features/F-13-posttrade-report/) — VWAP, IS, fill rate.
- [F-15. Backtest / Replay](../02-system/features/F-15-backtest-replay/) — parity replay.
- [F-17. Monitoring](../02-system/features/F-17-monitoring-and-alerts/) — alerts on slippage / fill rate.

## Source Fragments

- IN-005 §1 «ExecutionReports (ClickHouse execution_reports)»
- IN-005 §6 «slippageBps», «HedgePnL»
- IN-005 §10.1 (Backtest parity)
- IN-005 §11 DoD «ClickHouse хранит execution_reports с retention 90+ дней»
