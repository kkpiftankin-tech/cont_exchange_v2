---
id: DOC-DATA-REPLAY-SUMMARIES
phase: 07-data
status: implemented
owner: core-team
source:
  - IN-006 § replay_summaries (PostgreSQL)
related:
  - docs/02-system/features/F-15-backtest-replay/
  - cpp/backtest/migrations/postgres/001_f15_replay_schema.sql
---

# PostgreSQL: replay_summaries

> **Источник истины DDL:** [cpp/backtest/migrations/postgres/001_f15_replay_schema.sql](../../cpp/backtest/migrations/postgres/001_f15_replay_schema.sql)

Агрегированные метрики одного завершившегося replay (`completed`,
`cancelled+partial`, опционально `failed+partial`).

## DDL (актуально в коде)

```sql
CREATE TABLE IF NOT EXISTS replay_summaries (
  summary_id              UUID PRIMARY KEY,
  session_id              UUID NOT NULL UNIQUE REFERENCES replay_sessions(session_id) ON DELETE CASCADE,
  total_pnl               NUMERIC(24,8),
  avg_is                  NUMERIC(24,8),
  avg_pnl                 NUMERIC(24,8),
  std_pnl                 NUMERIC(24,8),
  sharpe_ratio            NUMERIC(24,8),
  fill_rate               NUMERIC(12,8),
  avg_vwap                NUMERIC(24,8),
  avg_solve_time_ms       NUMERIC(24,8),
  max_drawdown            NUMERIC(24,8),
  total_batches           INTEGER,
  processed_batches       INTEGER NOT NULL DEFAULT 0,
  failed_batches          INTEGER NOT NULL DEFAULT 0,
  total_fill_events       INTEGER,
  partial                 BOOLEAN NOT NULL DEFAULT FALSE,
  avgis_rule              TEXT NOT NULL DEFAULT 'volume_weighted',
  decision_price_source   TEXT NOT NULL DEFAULT 'marketdata_mid_with_clearprice_fallback',
  no_requested_volume     BOOLEAN NOT NULL DEFAULT FALSE,
  metrics                 JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at              TIMESTAMPTZ NOT NULL DEFAULT now(),
  CHECK (processed_batches >= 0),
  CHECK (failed_batches >= 0),
  CHECK (total_batches IS NULL OR total_batches >= processed_batches)
);

CREATE INDEX IF NOT EXISTS replay_summaries_created_at_idx
  ON replay_summaries (created_at DESC);
```

## Семантика полей и формулы

| Поле                     | Тип               | Формула / источник                                                                                                  |
| ------------------------ | ----------------- | ------------------------------------------------------------------------------------------------------------------- |
| `total_pnl`              | NUMERIC(24,8)     | $\sum_i \Delta PnL_i$ по всем processed-батчам                                                                       |
| `avg_pnl`                | NUMERIC(24,8)     | $E[\Delta PnL]$                                                                                                     |
| `std_pnl`                | NUMERIC(24,8)     | $\sigma(\Delta PnL)$                                                                                                |
| `sharpe_ratio`           | NUMERIC(24,8)     | $E[\Delta PnL] / \sigma(\Delta PnL)$. При $\sigma=0$ → 0 (F15-33).                                                  |
| `avg_is`                 | NUMERIC(24,8)     | Volume-weighted: $\sum (P^{\text{avg}}_i - P^{\text{decision}}_i) Q^{\text{exec}}_i / \sum Q^{\text{exec}}_i$ |
| `avgis_rule`             | TEXT              | `volume_weighted` (default). Альтернативы возможны через extension snapshot.                                       |
| `decision_price_source`  | TEXT              | `marketdata_mid_with_clearprice_fallback` (default). Описывает, как заполняется $P^{\text{decision}}$.            |
| `fill_rate`              | NUMERIC(12,8)     | $\sum \text{execqty} / \sum Q_{\max} \cdot 100\%$. При $\sum Q_{\max}=0$ → 0 (F15-34).                              |
| `avg_vwap`               | NUMERIC(24,8)     | $\sum \text{execqty} \cdot \text{execprice} / \sum \text{execqty}$                                                  |
| `avg_solve_time_ms`      | NUMERIC(24,8)     | Среднее `solve_time_ms` по processed-батчам                                                                          |
| `max_drawdown`           | NUMERIC(24,8)     | $\max_t (\text{peak}(t) - \text{cumPnL}(t))$                                                                         |
| `total_batches`          | INTEGER           | Запланированное число батчей                                                                                         |
| `processed_batches`      | INTEGER           | Фактически обработано                                                                                                |
| `failed_batches`         | INTEGER           | Сколько батчей soft-failed (`solver_error_flag=1`)                                                                   |
| `total_fill_events`      | INTEGER           | Кол-во записей в `replay_agentlogs.fills`                                                                            |
| `partial`                | BOOLEAN           | True если сессия cancelled/failed до полного завершения                                                              |
| `no_requested_volume`    | BOOLEAN           | True если $\sum Q_{\max}=0$ — fill_rate теряет смысл                                                                |
| `metrics`                | JSONB             | Расширенные метрики (per-symbol breakdown, hit rate, ...)                                                            |

## Идемпотентность

`UNIQUE` на `session_id` гарантирует, что у каждой сессии не более одного
summary. `INSERT ... ON CONFLICT (session_id) DO UPDATE` используется
`replay_step_journal::SaveSummary` для идемпотентности (например, при
retry того же session_id или при повторном recovery после crash).

## Retention

Сохраняется бессрочно (нужно для A/B compare через год+). Архивирование
вне scope MVP.

## Conflict Notes (DDL vs IN-006 source)

| ID          | Тема                                                                                                                            | Резолюция                                          |
| ----------- | ------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------- |
| CN-IN006-03 | IN-006 описывает `replay_summaries` с урезанным набором полей: `summaryid`, `avgis`, `totalpnl`, ... без `processed_batches`, `failed_batches`, `partial`, `avgis_rule`, `decision_price_source`, `no_requested_volume`, `metrics`. | Источник истины: код. Расширения нужны для partial summaries (cancel/failed) и для документирования decision price source. |

## Used In Features

- [F-15. Backtest / Replay](../02-system/features/F-15-backtest-replay/)

## Source Fragments

- IN-006 § replay_summaries
- Migration 001 (DDL источник истины)
