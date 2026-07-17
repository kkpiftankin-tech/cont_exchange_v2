---
id: DOC-DATA-VECTOR-FLOW-SEGMENTS
phase: 07-data
status: schema-ready-impl-pending
owner: core-team
source:
  - IN-014 §7.4 / §11.1 (F-10 DATA_MODEL)
related:
  - docs/07-data/vector-clearing-results.md
  - docs/02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md
  - contracts/proto/fob/marketdata/v1/vector_liquidity.proto
---

# PostgreSQL Tables: F-05A vectorization (OLTP)

> **Status:** ✅ DDL добавлена в [`infra/postgres/init.sql`](../../infra/postgres/init.sql) (F-05A секция). C++ repository-код — при реализации (T-F05A-104/205). Деньги/объёмы — `NUMERIC(38, 18)` (mirrors `fob.common.v1.Decimal`, §9).

## Owner

[market-data](../05-components/) — единственный writer (векторизация внешних стаканов).
Read: matching (через `marketdata.vectorized`, не напрямую), gateway (диагностика UI).

## Tables

### `asset_basis`

Упорядоченный справочник активов (ось векторов `w_i`).

| Column | Type | Notes |
| --- | --- | --- |
| `asset_basis_id` | `UUID PK` | |
| `assets` | `JSONB` | `[{"index":0,"asset":"BTC"}, ...]` |
| `num_assets` | `INT` | |
| `created_at` | `TIMESTAMPTZ` | |

### `vector_flow_segments`

Векторные flow-сегменты (столбцы `W`); каждый внешний order level — **отдельная строка**
(no synthetic pair book; provenance `venue_id`/`source_order_id` сохранён).

| Column | Type | Notes |
| --- | --- | --- |
| `segment_id` | `UUID PK` | |
| `batch_id` | `UUID` | |
| `venue_id`, `source_order_id`, `pair`, `side` | `TEXT` | `side` = bid/ask |
| `asset_basis_id` | `UUID FK` | → `asset_basis` |
| `w` | `JSONB` | вектор коэффициентов длины `num_assets` |
| `p_low`, `p_high`, `d_hl`, `q_rate`, `q_max`, `effective_price` | `NUMERIC(38,18)` | `p_low`=0, `p_high`=dHL |
| `source_timestamp`, `created_at` | `TIMESTAMPTZ` | |

Index: `idx_vector_flow_segments_batch (batch_id)`.

### `vectorization_runs`

Метаданные одного vectorization run (audit / replay): `run_id UUID PK`, `batch_id`,
`asset_basis_id` (FK), `num_segments`, `num_assets`, `solver_config_version`, `created_at`.

### `venue_order_levels`

Сырые внешние order levels (опц., до векторизации): `id UUID PK`, `batch_id`, `venue_id`,
`source_order_id`, `pair`, `base_asset`, `quote_asset`, `side`, `price`, `effective_price`,
`quantity`, `remaining_quantity`, `fees_bps`, `latency_buffer_bps`, `slippage_buffer_bps`,
`source_timestamp` (все деньги `NUMERIC(38,18)`).

## Used In

- Feature: [F-05A](../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)
- Use cases: [UC-F05A-01 Vectorize](../02-system/use-cases/UC-F05A-01-vectorize-external-orderbook/use-case.md)
- Sequence: [SEQ-F05A-UC-F05A-01-services](../05-components/sequences/SEQ-F05A-UC-F05A-01-services.md)
- Proto: `contracts/proto/fob/marketdata/v1/vector_liquidity.proto`
- OLAP counterpart: [vector-clearing-results.md](vector-clearing-results.md)
