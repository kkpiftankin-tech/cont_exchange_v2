---
id: DOC-DATA-CE-AGENT-NODES
phase: 07-data
status: schema-pending-impl
owner: core-team
source:
  - CE_algorithm_spec.md §5 (хранилище), §A9 (clearing_trace)
  - CE_virtual_counterparties.md §11 (что хранить)
related:
  - docs/03-architecture/adr/ADR-055-ce-virtual-counterparties-agents.md
  - docs/03-architecture/adr/ADR-057-ce-three-level-position.md
  - docs/03-architecture/adr/ADR-058-ce-committed-inflight-state.md
  - docs/03-architecture/adr/ADR-059-ce-clearing-trace-audit.md
  - contracts/proto/fob/agent/v1/agent.proto
  - contracts/proto/fob/ledger/v1/ledger.proto
---

# Data: CE Virtual Counterparties (agents, nodes, tact trace)

> **Status:** ⧗ DDL — при реализации (T-CEA-002/202/401). Деньги/объёмы — `NUMERIC(38,18)`
> (PG) / `Decimal128(18)` (CH), mirror `fob.common.v1.Decimal` (§9). Все таблицы за флагом
> CE-агентов; при выключении не читаются (обратимость ADR-055…059).

## Owner

- PG `agent_state`, `node_balances`, `f05a_clearing_config` — writer: matching (agent_state,
  committed) + ledger (node_balances); config — admin.
- CH `clearing_trace`, `node_balances_history`, `agent_state_history` — writer: matching;
  read: frontend-api (BFF), observability.

## PostgreSQL (OLTP)

### `agent_state` (new — ADR-058)

Состояние агента между тактами (`committed` вместо cooldown-таймера).

| Column | Type | Notes |
| --- | --- | --- |
| `agent_id` | `TEXT PK` | ребро графа (тип+плечо+узлы) |
| `committed` | `NUMERIC(38,18)` | отправлено, не подтверждено |
| `assigned_last` | `NUMERIC(38,18)` | назначено последним тактом |
| `filled_cum` | `NUMERIC(38,18)` | накопленное исполнение |
| `updated_at` | `TIMESTAMPTZ` | |

Транзакционная граница: `committed += |f|` пишется в одной транзакции с эмиссией поручений
(T-CEA-204). Снятие — по `execution.reports`.

### `node_balances` (new — ADR-057)

Средний уровень позиции: остаток «валюта@площадка».

| Column | Type | Notes |
| --- | --- | --- |
| `account` | `TEXT` | `__ce_house__` (собственные) |
| `asset` | `TEXT` | "BTC" |
| `venue` | `TEXT` | "Binance" \| "__book__" |
| `qty` | `NUMERIC(38,18)` | факт |
| `target` | `NUMERIC(38,18)` | цель `Q*` |
| `in_transit` | `NUMERIC(38,18)` | в пути на приход |
| `updated_at` | `TIMESTAMPTZ` | |

PK `(account, asset, venue)`. Меняется только подтверждениями (`execution.reports`,
`ApplyNodeTransfer`), не решением клиринга.

### `f05a_clearing_config` (ALTER — +5 колонок, ADR-055/056/057)

Таблица уже существует ([`infra/postgres/init.sql:942`](../../infra/postgres/init.sql)) с
`batch_window_ms, stale_level_ms, venue_stale_ms`. Добавить:

| Column | Type | Notes |
| --- | --- | --- |
| `theta` | `NUMERIC(38,18)` | haircut глубины (0.3…0.7), ADR-055 |
| `dhl_fraction` | `NUMERIC(38,18)` | квадратичная цена использования (сейчас const 0.01) |
| `rho` | `NUMERIC(38,18)` | сдвиг якоря на остаток/committed |
| `z_limit` | `NUMERIC(38,18)` | агрегатный лимит `Z_a`, ADR-057 |
| `gamma` | `NUMERIC(38,18)` | неприятие риска (α = W/(γσ²τ)) |
| `ce_taker_fee_bps` | `NUMERIC(38,18)` | настраиваемая с фронта комиссия тейкера, bps, для мёртвой зоны `c = комиссия + ½·spread`. `<0` = из стакана venue; `0` = линейные кривые (нет полки-комиссии). market_data поллит (TTL 2с) → `BuildQuoteAgent.taker_fee_bps_override`. Задаётся `POST /api/vector-clearing/config` (вкладка Vector Clearing). Дефолт `-1`. |

Миграция — `ALTER TABLE ... ADD COLUMN`, не `CREATE` (Conflict Note CN-3).

## ClickHouse (OLAP)

### `clearing_trace` (new — ADR-059)

Одна строка на такт (аудит; long-retention). JSON-колонки по образцу
[`vector_clearing_results`](vector-clearing-results.md).

| Column | Type | Notes |
| --- | --- | --- |
| `batch_id` | `String` | ключ такта |
| `ts_window` | `DateTime64(3)` | момент окна |
| `window_ms` | `UInt32` | |
| `venues` | `String (JSON)` | `[{venue,symbol,snapshot_ts,age_ms,included,exclude_reason}]` |
| `agents` | `String (JSON)` | `[{agent_id,type,leg,anchor,alpha,dead_zone,flow,x_buy,x_sell,...}]` |
| `nodes` | `String (JSON)` | `[{node,price_pm,price_abs,balance_residual,qty_before,delta,target}]` |
| `book_levels`, `curve_points`, `marks` | `String (JSON)` | для панелей стакана/кривой |
| `invariants` | `String (JSON)` | `[{code,value,tolerance,passed}]` (И-Т1…И-Т6) |
| `pnl` | `String (JSON)` | `{gross,quad,fees,net,closed_form}` |
| `position` | `String (JSON)` | `{by_agent,by_node,by_asset}` |
| `orders` | `String (JSON)` | `[{agent_id,venue,side,qty,order_type,price,zone}]` |
| `timings_ms` | `String (JSON)` | стадии конвейера |

Engine `MergeTree ORDER BY (ts_window, batch_id)`, TTL 30 d.

### `node_balances_history` (new — ADR-057)

Остатки узлов во времени (панель В-8).

| Column | Type |
| --- | --- |
| `asset`, `venue` | `LowCardinality(String)` |
| `qty`, `target`, `in_transit` | `Decimal128(18)` |
| `event_time_ms` | `DateTime64(3)` |

### `agent_state_history` (new — ADR-058)

`agent_id LowCardinality(String)`, `committed Decimal128(18)`, `assigned Decimal128(18)`,
`event_time_ms DateTime64(3)`.

### `vector_flow_segments_history` (ALTER — +9 колонок, T-CEA-001)

BFF уже селектит 9 колонок ([`server.js:6576`](../../frontend/api/server.js)), которых нет в
таблице → `/api/vector-clearing/detail` секция 1 падает. Добавить синхронно с писателем
[`clickhouse_vector_segment_storage.cpp`](../../cpp/market_data/src/infra/clickhouse/clickhouse_vector_segment_storage.cpp):

| Column | Type |
| --- | --- |
| `seg_index` | `UInt16` |
| `anchor`, `slope`, `q_min`, `alpha_ext`, `alpha_t`, `beta_t`, `theta` | `Decimal128(18)` |
| `translator_model` | `LowCardinality(String)` |

Проверить: у некоторых сред таблица уже могла получить часть колонок — реальный дефицит
подтвердить перед ALTER (Conflict Note: `/detail` в текущей сессии отдавал 200 по cePosition,
но сегментная секция могла падать отдельно).

## Used In

- Feature: [F-05A](../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)
- Use case: [UC-F05A-06](../02-system/use-cases/UC-F05A-06-ce-agent-tact-clearing/use-case.md)
- Sequence: [SEQ-F05A-UC-F05A-06-services](../05-components/sequences/SEQ-F05A-UC-F05A-06-services.md)
- Tasks: [F-05A-CE-agents.tasks.md](../implementation-plan/F-05A-CE-agents.tasks.md) (T-CEA-001/002/202/401)

## Conflict Notes (F-18 v2, 2026-09-15)

Источник конфликта — новая постановка алгоритма
[`incoming-docs/2026-09-15-CE_algorithm_v2.md`](../../incoming-docs/2026-09-15-CE_algorithm_v2.md)
(A0–A8). Она **пересматривает** модель, зафиксированную в этом файле (ADR-055…059). Ниже —
что именно superseded; каноническая v2-схема позиции — в
[`ce-agent-position.md`](ce-agent-position.md), решение — под ADR-061 (TODO, supersedes
ADR-057/058). Старые описания НЕ удаляются (правило истории ADR, CLAUDE.md §3.3) — помечаются.

- **CN-A. `target` / STOCK-агенты (плечи запаса) / `__book__`-узел — SUPERSEDED.**
  В v2 нет ни цели `target` (казначейство), ни агентов-плеч запаса, ни book-узла. Узлы — только
  «актив@площадка». Позиция агента = накопленный неисполненный поток `c_j` (стартует с нуля,
  знаковая), а не `владение − цель`. Поэтому:
  - `agent_state.committed` (ADR-058) → это **и есть** позиция `c_j`; отдельного `committed`
    больше нет. Заменяется таблицей `ce_agent_position` (см. `ce-agent-position.md`).
  - `node_balances.target` (ADR-057) → в v2 всегда `0` (цели нет). `NodeBalance.target` в proto
    помечается `[deprecated = true]`, физическое удаление номера — после стабилизации v2.
  - `venue = '__book__'` — в v2 не эмитится (граф = только venue-узлы + один «счёт дома»,
    невязка-столбец узла-нумерария, не агент).

- **CN-B. `clearing_trace` должен стать `ReplacingMergeTree` — MergeTree ломает идемпотентность
  replay.** Текущий движок `MergeTree ORDER BY (ts_window, batch_id)` при replay того же такта
  (F-15 backtest re-run того же `batch_id`) **продублирует** строку, нарушая правило idempotent
  ingestion. v2: `ReplacingMergeTree(ingested_at) ORDER BY (batch_id, ts_window)` — дедупликация
  по такту (`batch_id` — естественный business-key повторного прогона). JSON-поля также
  пересматриваются под v2 (`agents[]`: `+sigma_star, c_before, c_after, q_band, emitted_qty`,
  `−type/leg/target`; `nodes[]`: `−target/delta`; `+house`; инварианты `+I-BAND/I-OWNERSHIP/
  I-KERNEL`; `pnl`: `+plan_fact_gap`). Детали — при реализации кода.

- **CN-C. `agent_state` / `node_balances` НИКОГДА не создавались в `init.sql` — миграция
  документационная.** Таблицы ADR-057/058 существуют только в этом документе и ADR, реальной DDL
  для сноса нет. Поэтому «миграция» на v2 не разрушает живую БД: `ce_agent_position` — **новая**
  таблица (не `ALTER` существующей), а superseded-описания выше остаются как история.

См. также каноническую v2-схему: [`ce-agent-position.md`](ce-agent-position.md); ADR-061 (TODO).
