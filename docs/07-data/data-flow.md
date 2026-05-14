---
id: DOC-DATA-FLOW
phase: 07-data
status: draft
owner: core-team
source:
  - IN-001 «Сводная карта Сервис → БД → Операции»
related:
  - docs/07-data/oltp-schema.md
  - docs/07-data/olap-schema.md
  - docs/06-api/messaging/topics.md
---

# Data Flow

Карта потоков данных между сервисами, Kafka топиками, PostgreSQL (OLTP) и ClickHouse (OLAP).

## Сводная таблица «Сервис × Хранилище»

| Сервис | PostgreSQL (OLTP) | ClickHouse (OLAP) | Redis | Kafka |
| --- | --- | --- | --- | --- |
| Auth & Identity | R/W: `users`, `sessions` | — | (опц.) sessions | — |
| API Gateway | R: `users`, `sessions`, `accounts`, `flow_orders`, `positions` | — | — | — |
| Order Flow | (planned) R/W: `flow_orders` | — | — | P: `orders.normalized` |
| Matching Backend | R/W: `flow_orders`, `solver_config`; R: `risk_limits` | (via Kafka) | — | C: `orders.normalized`; P: `batch.outputs` |
| Risk Manager | R: `accounts`, `positions`, `flow_orders`, `risk_limits`; W: `risk_snapshots` | R: `risk_events` (offline-тюнинг) | — | C: `batch.outputs`; P: `risk.alerts`, `execution.intents` |
| Collateral & Ledger | R/W: `accounts`, `positions`, `collateral_transfers`, `risk_snapshots` | — | — | C: `batch.outputs`, `execution.reports` |
| Market Data Service | — | W: `marketdata`; R: `batch_results`, `fills` | R/W: ticker cache | C: `marketdata.raw`, `batch.outputs` |
| External Venues Connector | — | W: `marketdata`, `execution_reports` (через Kafka) | — | C: `execution.intents`; P: `marketdata.raw`, `execution.reports` |
| Backtest & Replay | R: `solver_config` | R: `fills`, `batch_results`, `marketdata`, `agent_logs`, `risk_events`, `execution_reports` | — | C: Kafka archive |
| Observability & Reporting | R: `risk_snapshots` | R: все таблицы; W: `risk_events`, `agent_logs` (через Kafka) | — | C: `risk.alerts`, `batch.outputs`, `execution.reports` |
| Blockchain / Custody Adapter | R/W: `collateral_transfers` | — | — | (опц.) P/C |

P = producer, C = consumer, R = read, W = write.

## Поток 1: создание FlowOrder

```
UI/API
  → API Gateway (REST/WS)
  → Order Flow (gRPC)
    → Risk Manager (gRPC PreTradeCheck)
      → PostgreSQL: risk_limits, accounts, positions (R)
    → Collateral & Ledger (gRPC Reserve)
      → PostgreSQL: accounts (R/W)
  → PostgreSQL: flow_orders (INSERT)
  → Kafka: orders.normalized (PRODUCE)
```

Подробности: [SEQ-F02-UC-F02-01-services](../05-components/sequences/SEQ-F02-UC-F02-01-services.md).

## Поток 2: batch clearing

```
Matching Backend (timer)
  ← Kafka: orders.normalized (CONSUME active state)
  ← PostgreSQL: solver_config (R)
  → solver.run() → BatchResult
  → Kafka: batch.outputs (PRODUCE)
    ↓
    ├→ Collateral & Ledger
    │   → PostgreSQL: accounts, positions (R/W idempotent by batch_id+order_id)
    │
    ├→ Risk Manager
    │   → PostgreSQL: positions, risk_snapshots (R/W)
    │   → Kafka: risk.alerts (PRODUCE if breach)
    │
    └→ Observability & Reporting
        → ClickHouse: fills, batch_results (W via Kafka engine)
```

Подробности: [SEQ-F04-UC-F04-01-services](../05-components/sequences/SEQ-F04-UC-F04-01-services.md).

## Поток 3: market data (внешний)

```
CEX/DEX
  ← External Venues Connector (subscribe)
  → Kafka: marketdata.raw (PRODUCE)
    ↓
    ├→ Market Data Service
    │   → Redis: last_ticker (W)
    │   → ClickHouse: marketdata (W via Kafka engine)
    │
    └→ Matching Backend (планируется — для reference price)

UI ← API Gateway (WS) ← Market Data Service
```

Подробности: [SEQ-F05-UC-F05-01-services](../05-components/sequences/SEQ-F05-UC-F05-01-services.md), [SEQ-F11-UC-F11-01-services](../05-components/sequences/SEQ-F11-UC-F11-01-services.md).

## Поток 4: execution hedge

```
Risk Manager / Matching Backend
  → Kafka: execution.intents (PRODUCE)
    ↓
    External Venues Connector (CONSUME)
      → CEX/DEX: child order
      ← venue: fill/reject
    → Kafka: execution.reports (PRODUCE)
      ↓
      ├→ Collateral & Ledger
      │   → PostgreSQL: accounts.venue_allocated (W)
      │
      └→ Observability & Reporting
          → ClickHouse: execution_reports (W via Kafka engine)
```

Подробности: [SEQ-F12-UC-F12-01-services](../05-components/sequences/SEQ-F12-UC-F12-01-services.md).

## Поток 5: deposit / withdraw

```
UI → API Gateway → Collateral & Ledger
  → Blockchain / Custody Adapter
    → blockchain/custody: tx (initiated)
  → PostgreSQL: collateral_transfers (INSERT status=pending)

Blockchain / Custody Adapter
  ← blockchain confirmation
  → PostgreSQL: collateral_transfers (UPDATE status=confirmed)
  → PostgreSQL: accounts (UPDATE free_balance)
```

Подробности: [SEQ-F14-UC-F14-01-services](../05-components/sequences/SEQ-F14-UC-F14-01-services.md).

## Поток 6: replay / backtest

```
Researcher
  → Backtest & Replay Engine
    ← ClickHouse: fills, batch_results, marketdata, agent_logs (R)
    ← PostgreSQL: solver_config (R, для подстановки альтернативных версий)
    → re-run Matching/Risk/Ledger in deterministic mode
    → ClickHouse: agent_logs (W if shadow run)
  → Observability & Reporting (сравнение и diff-отчёт)
```

## Принципы

1. **At-least-once + idempotency** на всех Kafka-flow. Ключи идемпотентности: `batch_id+order_id`, `intent_id`, `request_id`.
2. **Транзакционность** — все Ledger/Risk операции через ACID PostgreSQL.
3. **Replay-friendly** — Kafka long-retention + детерминированный solver обеспечивают повторяемость.
4. **OLTP separation** — никаких аналитических запросов к PostgreSQL под нагрузкой. Аналитика идёт в ClickHouse.

## Связанные документы

- [topics.md](../06-api/messaging/topics.md) — Kafka топики.
- [oltp-schema.md](oltp-schema.md), [olap-schema.md](olap-schema.md).
- Sequence diagrams: [`../05-components/sequences/`](../05-components/sequences/).

## Source Fragments

- IN-001-FR-031
