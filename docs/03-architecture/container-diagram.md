---
id: DOC-ARCH-CONTAINER
phase: 03-architecture
status: draft
owner: core-team
source:
  - IN-001 «Контейнеры системы (компоненты + БД + брокер)»
related:
  - docs/03-architecture/context-diagram.md
  - docs/05-components/components-overview.md
  - docs/07-data/data-overview.md
---

# C4 — Container Diagram (C2)

Внутренние контейнеры **Continuous Exchange System** на уровне сервисов, хранилищ и брокера.

## Diagram

```mermaid
graph TB
    UI[Web UI / Trading Frontend]

    subgraph edge["Edge"]
        GW[API Gateway<br/>REST + WebSocket]
        AUTH[Auth & Identity]
    end

    subgraph core["Core trading"]
        OF[Order Flow]
        MATCH[Matching Backend<br/>FOB Core]
        RISK[Risk Manager]
        LDG[Collateral & Ledger]
        MD[Market Data Service]
    end

    subgraph integration["Integration"]
        EV[External Venues Connector]
        CUSTADAPTER[Blockchain / Custody Adapter]
    end

    subgraph analytics["Analytics & operator"]
        BT[Backtest & Replay Engine]
        OBS[Observability & Reporting]
    end

    subgraph stores["Stores"]
        PG[(PostgreSQL<br/>OLTP)]
        CH[(ClickHouse<br/>OLAP)]
        REDIS[(Redis<br/>cache)]
        KAFKA[(Kafka / Redpanda<br/>event bus)]
    end

    UI -->|REST| GW
    UI <-->|WebSocket| GW
    GW -->|gRPC| AUTH
    GW -->|gRPC| OF
    GW -->|gRPC| LDG
    GW -->|gRPC| MD
    GW -->|gRPC| OBS

    OF -->|gRPC| RISK
    OF -->|gRPC| LDG
    OF -->|produce| KAFKA
    KAFKA -->|orders.normalized| MATCH

    MATCH -->|produce batch.outputs| KAFKA
    KAFKA -->|batch.outputs| LDG
    KAFKA -->|batch.outputs| RISK
    KAFKA -->|batch.outputs| OBS

    RISK -->|produce risk.alerts| KAFKA
    KAFKA -->|risk.alerts| OBS
    KAFKA -->|risk.alerts| OF

    EV -->|produce marketdata.raw| KAFKA
    KAFKA -->|marketdata.raw| MD
    MD -->|set/get| REDIS

    RISK -->|produce execution.intents| KAFKA
    KAFKA -->|execution.intents| EV
    EV -->|produce execution.reports| KAFKA
    KAFKA -->|execution.reports| LDG

    AUTH -->|sessions, users| PG
    OF -->|flow_orders| PG
    RISK -->|risk_limits, risk_snapshots| PG
    LDG -->|accounts, positions, collateral_transfers| PG
    MATCH -->|solver_config| PG

    MATCH -->|fills, batch_results via Kafka| CH
    OBS -->|risk_events, agent_logs| CH
    EV -->|marketdata, execution_reports via Kafka| CH

    LDG <-->|deposit/withdraw| CUSTADAPTER
    CUSTADAPTER <-->|on-chain ops| KAFKA

    BT -->|read historical| CH
    BT -->|read solver_config| PG
    BT -->|deterministic re-run| MATCH

    OBS -->|metrics, logs| KAFKA
```

## Контейнеры

| # | Container | Назначение | Технологии | Stores |
| --- | --- | --- | --- | --- |
| 1 | **Web UI / Trading Frontend** | UI: ввод заявок, графики, портфель, отчёты, операторская панель | (planned) React/TS | — |
| 2 | **API Gateway** | Edge HTTP/WebSocket → внутренний gRPC; auth, rate-limit, маршрутизация | C++20 (Crow + gRPC) | — |
| 3 | **Auth & Identity** | Регистрация, логин, сессии, роли, KYC-status | (planned) C++/Go | PostgreSQL (users, sessions) |
| 4 | **Order Flow** | Lifecycle FlowOrder: pre-trade, reserve, publish | C++20 + gRPC + Kafka | — |
| 5 | **Matching Backend (FOB Core)** | Solver равновесной цены/скорости, формирование `BatchResult` | C++20 | Kafka in/out, PG (solver_config) |
| 6 | **Risk Manager** | Pre/post-trade risk, kill-switch | C++20 + gRPC + Kafka | PG (risk_limits, risk_snapshots) |
| 7 | **Collateral & Ledger** | Балансы, резервы, применение fills | C++20 + gRPC + Kafka | PG (accounts, positions, collateral_transfers) |
| 8 | **Market Data Service** | Кэш тикеров, streaming UI/клиентам | C++20 + Kafka | Redis (ticker cache), ClickHouse (history) |
| 9 | **External Venues Connector** | Адаптеры CEX/DEX/AMM, `marketdata.raw`, hedge | C++20 (planned) | — |
| 10 | **Backtest & Replay Engine** | Воспроизведение истории, сравнение политик | (planned) Python/C++ | ClickHouse (read) |
| 11 | **Observability & Reporting** | Метрики, логи, отчёты, регуляторные выгрузки | C++20 | ClickHouse (write) |
| 12 | **Message Broker (Kafka/Redpanda)** | Шина событий | Redpanda 23.x | — |
| 13 | **Blockchain / Custody Adapter** | On-chain deposits / withdrawals, custody sync | (planned) | — |

## Хранилища

| Store | Тип | Назначение | Источник истины |
| --- | --- | --- | --- |
| **PostgreSQL** | OLTP | Пользователи, заявки, балансы, лимиты, snapshots | yes |
| **ClickHouse** | OLAP | Fills, batch_results, marketdata, risk_events, agent_logs, execution_reports | yes (для аналитики/replay) |
| **Redis** | cache | Last tickers, sessions (опц.) | no (derivable) |
| **Kafka / Redpanda** | event bus | orders.normalized, batch.outputs, risk.alerts, marketdata.raw, execution.intents, execution.reports | yes (audit / replay) |

Подробности хранилищ — в [`../07-data/`](../07-data/).

## Потоки данных (ключевые)

| Поток | Direction | Channel | Документ |
| --- | --- | --- | --- |
| CreateFlowOrder | UI → GW → OF | REST → gRPC | [SEQ-F02-UC-F02-01-services](../05-components/sequences/SEQ-F02-UC-F02-01-services.md) |
| BatchClearing | внутренний таймер | Kafka | [SEQ-F04-UC-F04-01-services](../05-components/sequences/SEQ-F04-UC-F04-01-services.md) |
| Live MD | EV → Kafka → MD → UI | Kafka + WS | [SEQ-F05-UC-F05-01-services](../05-components/sequences/SEQ-F05-UC-F05-01-services.md) |
| Pre-trade risk | OF → RISK | gRPC | [SEQ-F07-UC-F07-01-services](../05-components/sequences/SEQ-F07-UC-F07-01-services.md) |
| Execution hedge | SRC → EV → CEX | Kafka + venue SDK | [SEQ-F12-UC-F12-01-services](../05-components/sequences/SEQ-F12-UC-F12-01-services.md) |

## Связанные документы

- [context-diagram.md](context-diagram.md) — выше уровнем (C1).
- [components-overview.md](../05-components/components-overview.md) — текстовое описание каждого компонента.
- [communication.md](communication.md) — event-driven правила.
- Per-topic: [`../06-api/messaging/topics.md`](../06-api/messaging/topics.md).
- Data: [`../07-data/data-overview.md`](../07-data/data-overview.md).

## Source Fragments

- IN-001-FR-024
