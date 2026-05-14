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

Все исполнения (FillEvent) — основная аналитическая таблица.

| Поле | Тип | Описание |
| --- | --- | --- |
| `fill_id` | `UUID` | |
| `batch_id` | `UUID` | К какому batch относится |
| `order_id` | `UUID` | Заявка, по которой произошло |
| `user_id` | `UUID` | Владелец заявки |
| `symbol` | `String` | |
| `side` | `Enum8('buy'=1,'sell'=2)` | |
| `asset_legs` | `String` | JSON (для портфельных) |
| `exec_qty` | `Float64` | |
| `exec_price` | `Float64` | |
| `liquidity_source` | `Enum8('internal'=1,'cex_hedge'=2,'dex_hedge'=3,'epsilon_mm'=4)` | Кто предоставил ликвидность |
| `fees` | `Float64` | |
| `timestamp` | `DateTime64(3)` | |

`PARTITION BY toYYYYMMDD(timestamp)`, `ORDER BY (symbol, timestamp, fill_id)`.

**Сервисы-потребители:**

- **Observability & Reporting** (R) — VWAP, IS, fill rate, PnL агрегаты.
- **Backtest & Replay** (R) — replay для сравнения политик.
- **Market Data Service** (R) — агрегация объёмов для витрины.

## Таблица `batch_results`

Диагностика каждого clearing-цикла.

| Поле | Тип | Описание |
| --- | --- | --- |
| `batch_id` | `UUID` | |
| `timestamp` | `DateTime64(3)` | |
| `clear_prices` | `String` | JSON: { symbol → clear_price } |
| `executed_rates` | `String` | JSON: { order_id → executed_rate } |
| `residual_norm` | `Float64` | Норма остатка солвера |
| `solve_time_ms` | `UInt32` | |
| `num_active_orders` | `UInt32` | |
| `solver_diagnostics` | `String` | JSON: дополнительные метрики |
| `config_version` | `UInt32` | Версия solver_config |

`PARTITION BY toYYYYMMDD(timestamp)`, `ORDER BY (timestamp, batch_id)`.

**Сервисы-потребители:**

- **Observability & Reporting** (R) — мониторинг SLA solver (solve time, residual).
- **Backtest & Replay** (R) — сравнение batch-метрик при разных конфигурациях.
- **Market Data Service** (R) — исторические графики clear_prices.

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

## Связанные документы

- [oltp-schema.md](oltp-schema.md) — PostgreSQL.
- [data-flow.md](data-flow.md) — Kafka → DB flow.
- [data-overview.md](data-overview.md) — карта.

## Source Fragments

- IN-001-FR-030
