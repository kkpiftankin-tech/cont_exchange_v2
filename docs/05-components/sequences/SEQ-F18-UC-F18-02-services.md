<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-F18-UC-F18-02-services
level: sea
---
-->

# SEQ-F18-UC-F18-02-services. Балансы → NOP → хедж: service view (ADR-054 §10)

## Type

Service-level Sequence (cross-component)

## Feature

- [F-18. CE Capital, Net Position & Position-Based Hedge](../../02-system/features/F-18-ce-capital-position-hedge/)

## Use Cases

- [UC-F18-01](../../02-system/use-cases/UC-F18-01-form-ce-capital/use-case.md) — капитал = house-аккаунт (ledger)
- [UC-F18-02](../../02-system/use-cases/UC-F18-02-derive-ce-position/use-case.md) — NOP (risk)
- [UC-F18-03](../../02-system/use-cases/UC-F18-03-hedge-net-position/use-case.md) — хедж по NOP

## Purpose

Разделение по ADR-054 §10: **ledger** владеет валютными остатками биржи
(house-аккаунт + venue-балансы), **risk** считает NOP и размер хеджа, **execution
router** (matching planner + venues) исполняет, **ledger** уменьшает NOP реальным
исполнением. Frontend читает у сервисов. Никакой отдельной книги/проекции в matching.

## Diagram

```mermaid
sequenceDiagram
    participant M as matching-fob-core
    participant KB as Kafka batch.outputs
    participant L as ledger
    participant R as risk-manager
    participant EP as execution-planning (router)
    participant V as external-venues
    participant KV as Kafka execution.venue
    participant F as frontend-api

    Note over L: старт — seed house-аккаунта из CE_CAPITAL_SEED_<CCY>
    M->>KB: BatchResult (fills)
    KB->>L: consume fills → двойная запись балансов (клиенты + house)
    Note over R: цикл риска / по триггеру
    R->>L: GetExchangeBalances (валютный вектор биржи)
    L-->>R: {own_total, venue_total, client_liability} по валюте
    R->>R: NOP_a = активы_a − обязательства_a ; |NOP_a| > θ_a ?
    alt превышение θ и CE_NET_HEDGE_ENABLED
        R->>R: размер хеджа (FLATTEN/TO_BAND), side (излишек→SELL/дефицит→BUY)
        R->>EP: hedge intent (asset, qty, side) → execution.intents
        EP->>V: routing plan (venue split) → child orders
        V->>KV: ExecutionReport (fill, fee)
        KV->>L: consume → venue_balances −= (двигает NOP к target), realized PnL
    else в пределах θ
        R->>R: хедж не нужен
    end
    F->>L: GetExchangeBalances (UI: валютный вектор)
    F->>R: GetRiskSnapshot (UI: NOP/exposure + hedge)
```

## Трассировка

### Contract binding

| # | Arrow | Transport | Contract |
| --- | --- | --- | --- |
| 1 | M → KB → L | Kafka | `batch.outputs` (fills двигают балансы) |
| 2 | R → L | gRPC | `fob.ledger.v1.LedgerService.GetExchangeBalances` |
| 3 | R → EP | Kafka | `execution.intents` — `fob.execution.v1.ExecutionIntent` |
| 4 | V → KV → L | Kafka | `execution.venue` — `fob.execution.v1.ExecutionReport` |
| 5 | F → L | gRPC | `LedgerService.GetExchangeBalances` |
| 6 | F → R | gRPC | `fob.risk.v1.RiskService.GetRiskSnapshot` (NOP/exposure) |

### Data binding

| Object | Store | Doc |
| --- | --- | --- |
| house-аккаунт (капитал+остатки) | PostgreSQL `accounts['__ce_house__']` | [ce-capital.md](../../07-data/ce-capital.md) |
| venue-балансы биржи | ledger `venue_balances` (persist/replay) | [ce-capital.md](../../07-data/ce-capital.md) |
| NOP (производная) | не хранится (risk) | [ce-position.md](../../07-data/ce-position.md) |

## Related Components

- [ledger](../ledger/overview.md), [risk-manager](../risk-manager/overview.md),
  [execution-planning](../execution-planning/overview.md), [external-venues](../external-venues/overview.md)

## Source Fragments

- ADR-054 §10 «Определение позиции (NOP) и размещение по сервисам»
