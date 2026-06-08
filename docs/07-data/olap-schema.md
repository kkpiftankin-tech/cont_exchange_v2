---
id: DOC-DATA-OLAP
phase: 07-data
status: draft
owner: core-team
source:
  - IN-001 «БД 2: ClickHouse (OLAP — аналитика и история)»
related:
  - docs/07-data/data-overview.md
  - docs/07-data/oltp-schema.md
---

# OLAP Schema (ClickHouse)

ClickHouse — хранилище **аналитики, истории и replay-данных**. Ingestion из Kafka через `Kafka` table engine. Партиционирование обычно по `toYYYYMMDD(timestamp)`, ORDER BY включает `timestamp`.

ClickHouse **не источник истины для оперативных балансов** — для них PostgreSQL (см. [oltp-schema.md](oltp-schema.md)).

## Таблица `fills`

Все исполнения (FillEvent) — основная аналитическая таблица. Канонический DDL — [`infra/clickhouse/init.sql`](../../infra/clickhouse/init.sql); поля ниже соответствуют тому, что пишет `ClickHouseBatchStorage` (приходит из origin/dev `cpp/market_data/src/infra/clickhouse_storage.cpp` в follow-up PR).

| Поле | Тип | Описание |
| --- | --- | --- |
| `batch_id` | `String` | К какому batch относится |
| `event_time_ms` | `Int64` | UNIX мс из BatchResult.meta |
| `order_id` | `String` | Заявка, по которой произошло |
| `user_id` | `String` | Владелец заявки |
| `symbol` | `String` | напр. `BTC/USDT` |
| `base` | `String` | базовый актив (BTC) |
| `quote` | `String` | котировочный актив (USDT) |
| `side` | `LowCardinality(String)` | `buy` / `sell` |
| `executed_qty` | `Float64` | |
| `price` | `Float64` | clearing-цена |
| `executed_notional` | `Float64` | `executed_qty * price` |
| `fee_amount` | `Float64` | |
| `fee_currency` | `String` | |
| `liquidity_source` | `String` | `internal` / `cex_hedge` / `dex_hedge` / `epsilon_mm` |
| `venue_id` | `String` | для hedge fill |
| `snapshot_id` | `String` | привязка к venue snapshot (audit) |
| `curve_id` | `String` | привязка к LiquidityCurve (audit) |
| `ingested_at` | `DateTime DEFAULT now()` | |

`ENGINE = MergeTree`, `ORDER BY (event_time_ms, batch_id, order_id)`.

**Conflict Note (C-FILLS-V2).** Ранняя doc-версия упоминала колонки `fill_id`, `asset_legs` (JSON), Enum-типы для side/liquidity_source и партиционирование `toYYYYMMDD(timestamp)`. Импортированная в этом PR схема (mirrors origin/dev's clickhouse_storage) использует развёрнутые поля per-leg (symbol/base/quote вместо asset_legs JSON), LowCardinality(String) вместо Enum8 (для гибкости при добавлении liquidity_source), и не имеет fill_id (composite key `(batch_id, order_id)` + multi-leg fills получают разные строки).

**Сервисы-потребители:**

- **Observability & Reporting** (R) — VWAP, IS, fill rate, PnL агрегаты.
- **Backtest & Replay** (R) — replay для сравнения политик.
- **Market Data Service** (R/W) — пишет данные из Kafka `fills`; читает для агрегации.

## Таблица `batchresults`

Диагностика каждого clearing-цикла. Канонический DDL — [`infra/clickhouse/init.sql`](../../infra/clickhouse/init.sql); поля соответствуют `ClickHouseBatchStorage` (приходит из origin/dev `cpp/market_data/src/infra/clickhouse_storage.cpp` в follow-up PR).

| Поле | Тип | Описание |
| --- | --- | --- |
| `batch_id` | `String` | |
| `event_time_ms` | `Int64` | |
| `source` | `String` | `matching` |
| `correlation_id` | `String` | trace |
| `partition_key` | `String` | |
| `residual_norm` | `Float64` | Норма остатка солвера |
| `solve_time_ms` | `UInt32` | |
| `num_active_orders` | `UInt32` | |
| `config_version` | `UInt32` | Версия solver_config |
| `solver_diagnostics_json` | `String` | JSON: дополнительные метрики |
| `clear_prices_json` | `String` | JSON: { symbol → clear_price } |
| `executed_rates_json` | `String` | JSON: { order_id → executed_rate } |
| `used_liquidity_json` | `String` | JSON: { liquidity_source → executed_qty } |
| `fills_count` | `UInt32` | |
| `ingested_at` | `DateTime DEFAULT now()` | |

`ENGINE = MergeTree`, `ORDER BY (event_time_ms, batch_id)`.

**Conflict Note (C-BR-NAMING).** Ранняя doc-версия использовала snake_case (`batch_results`, `timestamp`, `solver_diagnostics`). Импортированная схема использует `batchresults` (один-слитное), `event_time_ms` (UNIX мс вместо DateTime64), и `*_json` суффиксы для JSON-полей. Doc выровнен под код.

**Сервисы-потребители:**

- **Observability & Reporting** (R) — мониторинг SLA solver (solve_time_ms, residual_norm).
- **Backtest & Replay** (R) — сравнение batch-метрик при разных конфигурациях.
- **Market Data Service** (R/W) — пишет данные из Kafka `batch.outputs`; читает для исторических графиков.

## Таблица `marketdata`

Исторические рыночные данные (внутренние + внешние).

| Поле | Тип | Описание |
| --- | --- | --- |
| `timestamp` | `DateTime64(3)` | |
| `source` | `Enum8('internal'=1,'binance'=2,'coinbase'=3,'uniswap'=4,'curve'=5)` | |
| `symbol` | `String` | |
| `mid_price` | `Float64` | |
| `best_bid` | `Float64` | |
| `best_ask` | `Float64` | |
| `bid_depth_json` | `String` | JSON массив уровней |
| `ask_depth_json` | `String` | |
| `spread` | `Float64` | |
| `volume_24h` | `Nullable(Float64)` | |

`PARTITION BY toYYYYMMDD(timestamp)`, `ORDER BY (symbol, source, timestamp)`.

**Сервисы-потребители:**

- **Market Data Service** (W при ingestion, R для исторических графиков).
- **Backtest & Replay** (R) — основной источник исторических цен.
- **Observability & Reporting** (R) — анализ ликвидности.

## Таблица `risk_events`

Лог событий risk-движка.

| Поле | Тип | Описание |
| --- | --- | --- |
| `event_id` | `UUID` | |
| `timestamp` | `DateTime64(3)` | |
| `entity_id` | `String` | user_id, symbol или venue |
| `event_type` | `Enum8('pre_trade_reject'=1,'pre_trade_throttle'=2,'margin_call'=3,'liquidation'=4,'kill_switch'=5)` | |
| `order_id` | `Nullable(UUID)` | |
| `details` | `String` | JSON: какой лимит, на сколько, результат |
| `batch_id` | `Nullable(UUID)` | для post-trade |

`PARTITION BY toYYYYMMDD(timestamp)`, `ORDER BY (entity_id, timestamp)`.

**Сервисы-потребители:**

- **Observability & Reporting** (R) — dashboards, регуляторные выгрузки.
- **Backtest & Replay** (R) — воспроизведение поведения Risk Manager.
- **Risk Manager** (R) — offline-тюнинг лимитов.

## Таблица `execution_reports`

Отчёты по внешним сделкам (hedge).

| Поле | Тип | Описание |
| --- | --- | --- |
| `venue_order_id` | `String` | ID ордера на venue |
| `intent_id` | `UUID` | Связь с ExecutionIntent |
| `venue` | `String` | |
| `symbol` | `String` | |
| `side` | `Enum8('buy'=1,'sell'=2)` | |
| `filled_qty` | `Float64` | |
| `avg_price` | `Float64` | |
| `fees` | `Float64` | |
| `status` | `Enum8('filled'=1,'partially_filled'=2,'rejected'=3,'cancelled'=4)` | |
| `reject_reason` | `Nullable(String)` | |
| `timestamp` | `DateTime64(3)` | |

`PARTITION BY toYYYYMMDD(timestamp)`, `ORDER BY (venue, timestamp)`.

**Сервисы-потребители:**

- **Observability & Reporting** (R) — slippage, rejection rate, hedge PnL.
- **Backtest & Replay** (R) — воспроизведение hedge цепочек.
- **Collateral & Ledger** (R через Kafka) — применение к `venue_allocated`.

## Таблица `agent_logs`

State–action–reward логи агентских политик (matching, risk, collateral, execution).

| Поле | Тип | Описание |
| --- | --- | --- |
| `log_id` | `UUID` | |
| `agent_id` | `String` | matching, risk, collateral, execution, ... |
| `policy_version` | `String` | |
| `mode` | `Enum8('live'=1,'shadow'=2,'replay'=3)` | |
| `batch_id` | `Nullable(UUID)` | |
| `observation_blob` | `String` | JSON: состояние среды, features |
| `action_blob` | `String` | JSON: принятое решение |
| `reward_components` | `String` | JSON: декомпозиция reward |
| `outcome_blob` | `String` | JSON: fill quality, latency, PnL |
| `timestamp` | `DateTime64(3)` | |

`PARTITION BY toYYYYMMDD(timestamp)`, `ORDER BY (agent_id, timestamp)`.

**Сервисы-потребители:**

- **Backtest & Replay** (R) — основной: grid search, calibration, политика → новая версия.
- **Observability & Reporting** (R) — мониторинг reward-трендов.
- **Research Layer** (R) — Python-аналитика, оптимизация политик.

## Ingestion из Kafka

| Kafka topic | → ClickHouse table |
| --- | --- |
| `batch.outputs` | `fills`, `batch_results` |
| `marketdata.raw` | `marketdata` |
| `risk.alerts` | `risk_events` |
| `execution.reports` | `execution_reports` |
| (planned) `agent.logs` | `agent_logs` |

Реализация: ClickHouse `Kafka` engine table + materialized view → MergeTree target. См. [data-flow.md](data-flow.md).

## F-09: Batch/Combo/Multi-leg Orders (OLAP)

> Source: IN-011 (F-09 v2 corrected §14.2), [ADR-032](../03-architecture/adr/ADR-032-parent-child-order-model.md), [ADR-033](../03-architecture/adr/ADR-033-execution-groups-topic.md).
> Ingestion: `execution.groups` → `grouped_execution_events`, `grouped_ratio_deviation`; `fills` (расширенный) → `grouped_leg_fills`; computed → `grouped_quality_metrics`; `backtest.execution.groups` → `grouped_replay_results`.
> Полный DDL применяет devops #14 в `infra/clickhouse/init.sql`. Денежные/количественные поля — `Decimal128(18)` (CLAUDE.md §9).

### Таблица `grouped_execution_events`

История событий grouped execution: один `ExecutionGroup` на строку. Источник — `execution.groups` (producer `matching`, ADR-033).

```sql
CREATE TABLE IF NOT EXISTS grouped_execution_events (
    execution_group_id   String,
    batch_id             String,
    parent_order_id      String,
    user_id              String,
    combo_type           LowCardinality(String),
    execution_mode       LowCardinality(String),
    group_status         LowCardinality(String),
    atomicity_policy     LowCardinality(String),
    atomicity_scope      LowCardinality(String),
    fallback_action      LowCardinality(String),
    execution_scale      Decimal128(18),
    ratio_deviation_bps  Nullable(Int32),
    violated_constraints String,   -- JSON
    solver_diagnostics   String,   -- JSON: groupSolveTimeMs, bindingLegs[], bindingConstraints[]
    leg_count            UInt16,
    event_time_ms        Int64,
    ingested_at          DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(event_time_ms)
PARTITION BY toYYYYMMDD(toDateTime(intDiv(event_time_ms, 1000)))
ORDER BY (parent_order_id, execution_group_id, event_time_ms)
TTL toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL 365 DAY;
```

### Таблица `grouped_leg_fills`

История leg fills с привязкой к группе. Источник — расширенный топик `fills` (`parentOrderId`, `executionGroupId`, `legId`).

```sql
CREATE TABLE IF NOT EXISTS grouped_leg_fills (
    fill_id              String,
    execution_group_id   String,
    parent_order_id      String,
    leg_id               String,
    batch_id             String,
    user_id              String,
    instrument_symbol    LowCardinality(String),
    side                 LowCardinality(String),
    exec_qty             Decimal128(18),
    exec_price           Decimal128(18),
    exec_notional        Decimal128(18),
    group_policy         LowCardinality(String),
    liquidity_source     LowCardinality(String),
    venue_id             String,
    event_time_ms        Int64,
    ingested_at          DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(event_time_ms)
PARTITION BY toYYYYMMDD(toDateTime(intDiv(event_time_ms, 1000)))
ORDER BY (execution_group_id, leg_id, fill_id, event_time_ms)
TTL toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL 365 DAY;
```

### Таблица `grouped_quality_metrics`

Агрегаты качества по ExecutionGroup: combined VWAP/IS, ratio deviation. MV из `grouped_execution_events` + `grouped_leg_fills`.

```sql
CREATE TABLE IF NOT EXISTS grouped_quality_metrics (
    execution_group_id   String,
    parent_order_id      String,
    batch_id             String,
    user_id              String,
    combo_type           LowCardinality(String),
    atomicity_policy     LowCardinality(String),
    group_status         LowCardinality(String),
    execution_scale      Decimal128(18),
    combined_vwap        Decimal128(18),
    combined_notional    Decimal128(18),
    combined_is_bps      Nullable(Int64),
    ratio_deviation_bps  Nullable(Int32),
    fallback_action      LowCardinality(String),
    leg_count            UInt16,
    violated_constraint_count UInt16,
    solve_time_ms        UInt32,
    event_time_ms        Int64,
    ingested_at          DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(event_time_ms)
PARTITION BY toYYYYMMDD(toDateTime(intDiv(event_time_ms, 1000)))
ORDER BY (parent_order_id, execution_group_id, event_time_ms)
TTL toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL 365 DAY;
```

### Таблица `grouped_ratio_deviation`

История отклонений ratio/weight по batch cycle; выявление binding legs (AC-F09-002).

```sql
CREATE TABLE IF NOT EXISTS grouped_ratio_deviation (
    execution_group_id   String,
    parent_order_id      String,
    batch_id             String,
    user_id              String,
    leg_id               String,
    instrument_symbol    LowCardinality(String),
    target_weight        Nullable(Decimal128(18)),
    target_ratio         Nullable(Decimal128(18)),
    actual_exec_qty      Decimal128(18),
    actual_exec_notional Decimal128(18),
    deviation_bps        Int32,
    is_binding_leg       UInt8,
    event_time_ms        Int64,
    ingested_at          DateTime DEFAULT now()
)
ENGINE = MergeTree()
PARTITION BY toYYYYMMDD(toDateTime(intDiv(event_time_ms, 1000)))
ORDER BY (parent_order_id, event_time_ms, leg_id)
TTL toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL 180 DAY;
```

### Таблица `grouped_replay_results`

Результаты F-15 Backtest Replay для многоногих заявок; replay determinism (AC-F09-010). Источник — изолированный топик `backtest.execution.groups` (по аналогии с `backtest.execution.venue`).

```sql
CREATE TABLE IF NOT EXISTS grouped_replay_results (
    replay_session_id    String,
    execution_group_id   String,
    batch_id             String,
    parent_order_id      String,
    user_id              String,
    combo_type           LowCardinality(String),
    execution_mode       LowCardinality(String),
    group_status         LowCardinality(String),
    atomicity_policy     LowCardinality(String),
    execution_scale      Decimal128(18),
    ratio_deviation_bps  Nullable(Int32),
    combined_vwap        Nullable(Decimal128(18)),
    combined_notional    Nullable(Decimal128(18)),
    violated_constraints String,
    solver_diagnostics   String,
    leg_results          String,
    event_time_ms        Int64,
    ingested_at          DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(event_time_ms)
PARTITION BY toYYYYMMDD(toDateTime(intDiv(event_time_ms, 1000)))
ORDER BY (replay_session_id, parent_order_id, execution_group_id, event_time_ms)
TTL toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL 90 DAY;
```

### Kafka → ClickHouse (F-09)

| Kafka topic | → ClickHouse | Примечания |
| --- | --- | --- |
| `execution.groups` | `grouped_execution_events`, `grouped_ratio_deviation` | producer `matching`; key `parentOrderId` (ADR-033) |
| `fills` (расширенный) | `grouped_leg_fills` | поля `parentOrderId`/`executionGroupId`/`legId` |
| computed/MV | `grouped_quality_metrics` | из events + leg_fills |
| `backtest.execution.groups` | `grouped_replay_results` | только replay, изолирован от live |

## Связанные документы

- [oltp-schema.md](oltp-schema.md) — PostgreSQL.
- [data-flow.md](data-flow.md) — Kafka → DB flow.
- [data-overview.md](data-overview.md) — карта.

## Source Fragments

- IN-001-FR-030
- IN-011 §14.2 (F-09 grouped_* OLAP tables)
