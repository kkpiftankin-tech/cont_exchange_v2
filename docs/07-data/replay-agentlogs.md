---
id: DOC-DATA-REPLAY-AGENTLOGS
phase: 07-data
status: implemented
owner: core-team
source:
  - IN-006 § replay_agentlogs (ClickHouse)
related:
  - docs/02-system/features/F-15-backtest-replay/
  - cpp/backtest/src/infra/clickhouse_storage.cpp
---

# ClickHouse: replay_agentlogs

> **Источник истины DDL:** [cpp/backtest/src/infra/clickhouse_storage.cpp](../../cpp/backtest/src/infra/clickhouse_storage.cpp)
> метод `EnsureSchema()`. Таблица создаётся динамически при старте сервиса
> (см. Conflict Note CN-IN006-04). Имя таблицы конфигурируется через env
> `BACKTEST_CLICKHOUSE_REPLAY_AGENTLOGS_TABLE` (default `replay_agentlogs`).

Per-batch журнал `state-action-reward` + diagnostics. Используется UI
(пошаговая таблица), audit (поиск по `solver_error_flag`, `pnl`,
`fillrate`), RL training, и compare сессий.

## DDL (актуально в коде)

```sql
CREATE TABLE IF NOT EXISTS replay_agentlogs (
  session_id                       String,
  log_id                           String,
  original_batch_id                String,
  batch_seq                        UInt32,
  event_time_ms                    Int64,
  state_json                       String,
  action_json                      String,
  reward                           Float64,
  pnl                              Float64,
  is_value                         Float64,
  fill_rate                        Float64,
  fills_applied                    UInt32,
  solve_time_ms                    UInt32,
  residual_norm                    Float64,
  solver_error_flag                UInt8,
  risk_status                      LowCardinality(String),
  error_code                       String,
  error_details                    String,
  failure_component                LowCardinality(String),
  fills_json                       String,
  batch_result_json                String,
  metrics_json                     String,
  session_config_snapshot_version  UInt32 DEFAULT 1,
  ingested_at                      DateTime64(3) DEFAULT now64(3),
  INDEX idx_solver_error_flag solver_error_flag TYPE set(2) GRANULARITY 1,
  INDEX idx_fill_rate         fill_rate         TYPE minmax  GRANULARITY 1,
  INDEX idx_pnl               pnl               TYPE minmax  GRANULARITY 1
) ENGINE = ReplacingMergeTree(ingested_at)
ORDER BY (session_id, original_batch_id)
TTL toDateTime(event_time_ms / 1000) + INTERVAL 90 DAY DELETE;
```

## Поля

| Поле                              | Тип                          | Назначение                                                                                                  |
| --------------------------------- | ---------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `session_id`                      | String                       | Сессия (FK на PG `replay_sessions`)                                                                          |
| `log_id`                          | String                       | Уникальный ID записи (UUID); добавлено `ALTER ADD COLUMN IF NOT EXISTS`                                      |
| `original_batch_id`               | String                       | Соответствующий исторический batch_id; ключ для replace при retry                                            |
| `batch_seq`                       | UInt32                       | Sequence в рамках сессии (1-based)                                                                          |
| `event_time_ms`                   | Int64                        | Время оригинального батча                                                                                    |
| `state_json` / `action_json`      | String (JSON)                | Snapshot state до батча / action агента                                                                      |
| `reward`                          | Float64                      | Согласно `reward_mode` (pnl, -is, hybrid)                                                                    |
| `pnl`, `is_value`, `fill_rate`    | Float64                      | Per-batch метрики                                                                                            |
| `fills_applied`                   | UInt32                       | Кол-во применённых fills в shadow ledger                                                                     |
| `solve_time_ms`                   | UInt32                       | Solver latency                                                                                               |
| `residual_norm`                   | Float64                      | Норма остатка солвера                                                                                        |
| `solver_error_flag`               | UInt8                        | 1 если soft failure (residual_norm > tolerance или solver throw)                                             |
| `risk_status`                     | LowCardinality(String)        | `ok` / `alert` / `rejected`                                                                                   |
| `error_code`, `error_details`     | String                       | Machine-readable + human readable error                                                                      |
| `failure_component`               | LowCardinality(String)        | `matching` / `risk` / `ledger` / `clickhouse` / `none`                                                       |
| `fills_json`                      | String (JSON)                | Полный список fills этого батча                                                                              |
| `batch_result_json`               | String (JSON)                | Полный BatchResult solver'а                                                                                  |
| `metrics_json`                    | String (JSON)                | Дополнительные метрики (per-symbol breakdown и т. д.)                                                        |
| `session_config_snapshot_version` | UInt32                       | Версия snapshot (для retry с тем же snapshot и обновлений)                                                   |
| `ingested_at`                     | DateTime64(3)                | Version column для ReplacingMergeTree                                                                        |

## Идемпотентность вставки

`ENGINE = ReplacingMergeTree(ingested_at) ORDER BY (session_id, original_batch_id)`
обеспечивает идемпотентный INSERT: повторный прогон этого же batch_id для
этой же session (например, при resume после restart) перезапишет
предыдущую запись при merge (или через `SELECT ... FINAL` на чтение).

## Retention

TTL = 90 дней по полю `event_time_ms`. Настраиваемый через env
`BACKTEST_CLICKHOUSE_REPLAY_AGENTLOGS_RETENTION_DAYS`. ALTER применяется
при старте сервиса (см. `apply_replay_agentlogs_ttl` в clickhouse_storage.cpp).

## INSERT format

`INSERT INTO replay_agentlogs FORMAT JSONEachRow` (см. line 526 в
clickhouse_storage.cpp).

## Conflict Notes (DDL vs IN-006 source)

| ID          | Тема                                                                                                                                                                                                                  | Резолюция                                                              |
| ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- |
| CN-IN006-01 | IN-006 описывает таблицу со столбцами `logid`, `sessionid`, `batchseq`, `originalbatchid`, `state`, `action`, `reward`, `pnl`, `is_value`, `fillrate`, `solvetime_ms`, `residualnorm`, `fills`, `batchresult`, `timestamp`, `ENGINE = MergeTree() PARTITION BY toYYYYMM(timestamp) ORDER BY (sessionid, batchseq)`. Код использует snake_case, ReplacingMergeTree, расширенный набор полей (см. выше), и ORDER BY `(session_id, original_batch_id)` для idempotency. | Источник истины: код (deployed). IN-006 — initial spec, расширен. |
| CN-IN006-04 | IN-006 предполагает, что DDL должно быть в `infra/clickhouse/init.sql`. Фактически — создаётся динамически из `clickhouse_storage.cpp::EnsureSchema()`.                                                                | Документ зафиксировал место создания. Возможный future ADR — переместить в init.sql для idempotency между сервисами. |

## Used In Features

- [F-15. Backtest / Replay](../02-system/features/F-15-backtest-replay/)

## Source Fragments

- IN-006 § replay_agentlogs (ClickHouse)
- cpp/backtest/src/infra/clickhouse_storage.cpp (источник истины DDL)
