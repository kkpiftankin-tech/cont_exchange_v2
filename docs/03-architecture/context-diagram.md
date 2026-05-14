---
id: DOC-ARCH-CONTEXT
phase: 03-architecture
status: draft
owner: core-team
source:
  - IN-001 §«Внешние участники»
related:
  - docs/02-system/actors.md
  - docs/03-architecture/container-diagram.md
---

# C4 — Context Diagram (C1)

Внешние участники и внешние системы, взаимодействующие с **Continuous Exchange System** (CES).

## Diagram

```mermaid
graph TB
    subgraph external_users["External users"]
        LT[Liquidity Trader]
        MM[Market Maker / LP]
        INT[Client Integrator<br/>/ Provider / Broker]
        OP[Operator / Admin]
        COMP[Compliance Officer]
        RES[Researcher / Quant]
    end

    subgraph external_systems["External systems"]
        CEX[CEX<br/>Binance / Coinbase / Bybit]
        DEX[DEX / AMM<br/>Uniswap-like]
        CUST[Custody Provider]
        CHAIN[Blockchain Network]
        KYC[KYC / AML Provider]
        REG[Regulator]
        ANALYTICS[Analytics Platform<br/>external]
    end

    CES["Continuous Exchange System<br/>(Flow Order Book)"]

    LT -->|FlowOrder, ams, cancels| CES
    MM -->|CSLO curves| CES
    INT -->|API: orders, status| CES
    OP -->|admin actions| CES
    COMP -->|report queries| CES
    RES -->|backtest / replay| CES

    CES <-->|market data, hedge child orders| CEX
    CES <-->|market data, hedge child orders| DEX
    CES <-->|deposits / withdrawals| CUST
    CUST <-->|on-chain transfers| CHAIN
    CES -->|user verification, txn monitoring| KYC
    CES -->|reports, audit logs| REG
    CES -->|anonymized stats| ANALYTICS
```

## Описание

CES — единое целое с точки зрения внешнего наблюдателя. Внутренние компоненты раскрыты на уровне C2 — см. [container-diagram.md](container-diagram.md).

### Внешние пользователи

| Actor | Поток в CES | Поток из CES |
| --- | --- | --- |
| **Liquidity Trader** | заявки `FlowOrder` (single / portfolio) | preview VWAP/IS, fills, статусы, отчёты, balances |
| **Market Maker / LP** | CSLO-кривые (P_L, P_H, Q, U, slope), inventory caps | fills по кривой, PnL, inventory exposure, adverse selection |
| **Client Integrator** | заявки от множества клиентов (агрегатор) | агрегированные fills, отчёты, лимиты |
| **Operator / Admin** | конфигурация (risk_limits, solver_config, fees), kill-switch | dashboards, alerts, audit |
| **Compliance Officer** | запросы регуляторных выгрузок | trade logs, decisions, risk events |
| **Researcher** | backtest / replay scenarios | сравнение результатов |

### Внешние системы

| External system | Назначение | Bidirectional? |
| --- | --- | --- |
| **CEX** (Binance, Coinbase и т.д.) | reference prices, execution hedge target | yes |
| **DEX / AMM** (Uniswap, Curve) | on-chain liquidity «of last resort» | yes |
| **Custody Provider** | хранение активов, деньги клиентов | yes |
| **Blockchain** | settlement layer для on-chain ops | yes |
| **KYC / AML Provider** | верификация и мониторинг | mostly outbound |
| **Regulator** | получатель отчётности | outbound |
| **Analytics Platform** | публичная статистика | outbound |

## Что НЕ показано на C1

Этот уровень намеренно скрывает:

- API Gateway, Auth, Matching, Risk, Ledger и прочие внутренние сервисы;
- Kafka, PostgreSQL, ClickHouse, Redis;
- внутреннюю реализацию hedge / клиринга / лимитов.

Эти артефакты находятся на уровне C2 в [container-diagram.md](container-diagram.md), а пошаговое поведение — в use-case-/service-level sequence diagrams (см. [`../02-system/use-cases/`](../02-system/use-cases/), [`../05-components/sequences/`](../05-components/sequences/)).

## Связанные документы

- [actors.md](../02-system/actors.md) — детально по каждому actor.
- [container-diagram.md](container-diagram.md) — следующий уровень детализации.
- [architecture-overview.md](architecture-overview.md) — обзор архитектуры.

## Source Fragments

- IN-001-FR-021
