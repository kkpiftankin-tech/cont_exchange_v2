---
id: DOC-DATA-VECTOR-CLEARING-RESULTS
phase: 07-data
status: schema-ready-impl-pending
owner: core-team
source:
  - IN-014 §7.4 / §11.2 (F-10 DATA_MODEL)
related:
  - docs/07-data/vector-flow-segments.md
  - docs/07-data/olap-schema.md
  - docs/02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md
  - docs/03-architecture/adr/ADR-044-surplus-exchange-pnl-policy.md
---

# ClickHouse Tables: F-05A vector clearing (OLAP)

> **Status:** ✅ DDL добавлена в [`infra/clickhouse/init.sql`](../../infra/clickhouse/init.sql) (F-05A секция). Ingestion-код — при реализации (T-F05A-105/403, паттерн `ClickHouseBatchStorage::SaveExecutionGroup` из F-09). Деньги — `Decimal128(18)` (§9); нормы/диагностика — `Float64`.

## Owner

[market-data](../05-components/) — writer (ingestion `execution.groups` / vector diagnostics,
как grouped_* в F-09). Read: gateway (UI диагностика), backtest (replay сравнение), observability.

## Tables

### `vector_clearing_results`

Результат vector clearing: один ExecutionGroup (vector) на строку.
`ReplacingMergeTree(event_time_ms)` — идемпотентная ре-ingestion.

| Column | Type | Notes |
| --- | --- | --- |
| `batch_id`, `execution_group_id` | `String` | |
| `asset_basis_json`, `x_json`, `pi_json`, `residual_json` | `String` | JSON (Decimal как строки) |
| `residual_norm` | `Float64` | `‖Wx‖` (diagnostic) |
| `solver_status` | `LowCardinality(String)` | converged/degraded/failed |
| `surplus_json` | `String` | `[{asset, amount, allocation_policy}]` |
| `solver_diagnostics_json` | `String` | iterations, solveTimeMs, paramsVersion |
| `leg_count` | `UInt16` | |
| `event_time_ms` | `Int64` | |

TTL 365 дней; PARTITION по дню; ORDER BY `(execution_group_id, batch_id, event_time_ms)`.

### `surplus_events`

Surplus / EXCHANGE_PNL события ([ADR-044](../03-architecture/adr/ADR-044-surplus-exchange-pnl-policy.md)).
Один surplus-актив на строку. `MergeTree`.

| Column | Type | Notes |
| --- | --- | --- |
| `batch_id`, `execution_group_id` | `String` | |
| `asset` | `LowCardinality(String)` | |
| `amount` | `Decimal128(18)` | сумма surplus |
| `allocation_policy` | `LowCardinality(String)` | REJECT_IF_RESIDUAL / EXCHANGE_PNL / SURPLUS_ASSET / MM_LAST_RESORT |
| `event_time_ms` | `Int64` | |

### `vector_flow_segments_history`

История vectorized flow segments (столбцы `W`) для audit / replay:
`segment_id`, `batch_id`, `venue_id`, `source_order_id`, `pair`, `side`, `w_json`,
`p_high`/`d_hl`/`q_rate`/`q_max`/`effective_price` (`Decimal128(18)`), `event_time_ms`.
`ReplacingMergeTree`, TTL 180 дней.

## Used In

- Feature: [F-05A](../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)
- Use cases: [UC-F05A-02 Vector Clearing](../02-system/use-cases/UC-F05A-02-vector-clearing/use-case.md), [UC-F05A-03 Display](../02-system/use-cases/UC-F05A-03-display-vector-liquidity/use-case.md), [UC-F05A-05 Replay](../02-system/use-cases/UC-F05A-05-replay-vector-clearing/use-case.md)
- Sequences: [SEQ-F05A-UC-F05A-02-services](../05-components/sequences/SEQ-F05A-UC-F05A-02-services.md), [SEQ-F05A-UC-F05A-05-services](../05-components/sequences/SEQ-F05A-UC-F05A-05-services.md)
- ADR: [ADR-044 surplus policy](../03-architecture/adr/ADR-044-surplus-exchange-pnl-policy.md)
- OLTP counterpart: [vector-flow-segments.md](vector-flow-segments.md)
