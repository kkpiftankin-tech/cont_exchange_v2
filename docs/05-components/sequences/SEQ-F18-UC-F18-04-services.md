<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-F18-UC-F18-04-services
level: sea
---
-->

# SEQ-F18-UC-F18-04-services. Такт агентов и эмиссия по полосе: service view (v2)

## Type

Service-level Sequence (cross-component)

## Feature

- [F-18 (v2). CE Virtual Counterparties — Translator/Arbitrageur Agents, Signed Agent Position & Band Hedge](../../02-system/features/F-18-ce-capital-position-hedge/)

## Use Case

- [UC-F18-04](../../02-system/use-cases/UC-F18-04-ce-v2-agent-band-hedge/use-case.md)

## Purpose

Размещение по сервисам (проект. под ADR-061): **market_data** строит кривые агентов из
снимка (A0–A1); **matching** сдвигает якорь `σ*=m+ρ·c`, собирает `W`+house, клирит и
накапливает `c_j` (A2–A6); **ledger** — владелец знаковой позиции агента
(`ce_agent_position`, party_type AGENT) и узловых остатков; **risk** проверяет полосу
`±q`, считает размер (walk) и эмиттит заявку сверх полосы через `PreHedgeCheck` (A7);
**venues** исполняет, ledger подтверждает `qty[asset@venue]` + realized (A8). Декремент
`c_j` — при эмиссии (A7), не при исполнении. Владение меняют только клиент + исполнение.

## Diagram

```mermaid
sequenceDiagram
    participant MDS as market_data
    participant KM as Kafka marketdata.raw
    participant MB as matching
    participant KP as Kafka ce.position.delta
    participant L as ledger
    participant R as risk
    participant KI as Kafka execution.intents
    participant V as venues
    participant KV as Kafka execution.venue
    participant PG as PG ce_agent_position

    Note over MDS: A0-A1 — снимок обоих стаканов; кривые агентов (m, α, полка c)
    KM->>MDS: снимок стаканов (A0)
    MDS->>MB: VectorClearingInput (кривые переводчиков/арбитражёров)
    MB->>L: GetAgentPositions(agent_id, asset, venue) [gRPC]
    L->>PG: SELECT position (signed)
    L-->>MB: c_j по агентам
    MB->>MB: σ*_j = m_j + ρ_j·c_j (A2); собрать W + house-столбец (A3)
    MB->>MB: клиринг closed-form + OSQP cross-check (A4); |Wf|<1e-6, ≤95% (A5)
    MB->>MB: c_j += f_j (A6, наружу ничего)
    MB->>KP: AgentDelta batch (знаковая Δc_j по узлу)
    KP->>L: consume AgentDelta → upsert ce_agent_position (владение НЕ трогается)
    L->>PG: UPSERT position (guard last_batch_id)
    R->>L: GetAgentPositions (снимок после A6) [gRPC]
    L-->>R: c_j по агентам
    R->>R: для каждого агента |c_j| > q_j ? (A7; q_translator=18 / q_arbitrageur=30)
    alt |c_j| > q_j
        R->>MDS: глубина книги для walk (qty ← излишек·1000/walk, 2-3 итер) [gRPC]
        MDS-->>R: depth
        R->>R: PreHedgeCheck (reuse F-12)
        alt PreHedgeCheck ACCEPT
            R->>KI: ExecutionIntent (agent_id, reduce_by=qty, in_flight)
            R->>L: ApplyAgentPositionReduction(agent_id, -qty) [gRPC] — c_j уменьшен в A7
            L->>PG: UPDATE position -= qty
            KI->>V: consume ExecutionIntent → place order (in_flight)
            V->>KV: ExecutionReport (fill / partial / timeout)
            KV->>L: consume → qty[asset@venue] + realized (A8 confirm); позицию агента не трогать
        else PreHedgeCheck REJECT
            Note over R: intent не публикуется; c_j НЕ уменьшается; risk.alert
        end
    else |c_j| <= q_j
        Note over R: позиция внутри полосы — эмиссии нет
    end
```

## Трассировка

### Contract binding

| # | Arrow | Transport | Contract |
| --- | --- | --- | --- |
| 1 | KM → MDS | Kafka | `marketdata.raw` — сырой снимок стаканов (A0) |
| 2 | MDS → MB | internal / Kafka | `VectorClearingInput` (кривые агентов) — существующий F-05A путь |
| 3 | MB → L, R → L | gRPC | `fob.ledger.v1.LedgerService.GetAgentPositions` — **planned**, `docs/06-api/grpc/ledger-get-agent-positions.md` (TODO contract) |
| 4 | MB → KP → L | Kafka | `ce.position.delta` — `AgentDelta{agent_id, agent_kind, asset, venue, delta}` — **planned** (TODO contract) |
| 5 | R → MDS | gRPC | `fob.marketdata.v1.MarketDataService` (depth/walk) — существующий контракт |
| 6 | R → R | gRPC (internal) | `fob.risk.v1.RiskService.PreHedgeCheck` (reuse F-12) |
| 7 | R → KI → V | Kafka | `execution.intents` — `fob.execution.v1.ExecutionIntent` — существующий контракт |
| 8 | R → L (reduction) | gRPC | `LedgerService.ApplyAgentPositionReduction` — **planned** (TODO contract) |
| 9 | V → KV → L | Kafka | `execution.venue` — `fob.execution.v1.ExecutionReport` (A8 confirm) — существующий контракт |

### Data binding

| Object | Store | Doc |
| --- | --- | --- |
| позиция агента (signed) | PostgreSQL `ce_agent_position(agent_id, asset, venue, position, updated_at, last_batch_id)` | planned — `docs/07-data/ce-agent-position.md` (TODO schema) |
| конфиг такта | PostgreSQL `f05a_clearing_config` (+`ρ_Q`, `ρ_T`, `q_translator`, `q_arbitrageur`, `transfer_eta`) | planned |
| узловые остатки биржи | ledger `qty[asset@venue]` (владение) | planned — `docs/07-data/ce-agent-position.md` |
| перевозы арбитражёра | PostgreSQL `ce_transfer(id, asset, src, dst, qty, eta, tariff, status)` | planned (Э5) |
| трасса такта | ClickHouse `clearing_trace` (+`agent_id`, `c_j` до/после, house-невязка) | planned |

## Related Components

- [matching-fob-core](../matching-fob-core/overview.md), [ledger](../ledger/overview.md),
  [risk-manager](../risk-manager/overview.md), [external-venues](../external-venues/overview.md),
  [market-data](../market-data/overview.md)

## Related ADR

- ADR-061 (planned, supersede ADR-054/057; ревизии ADR-055/056/058).

## Source Fragments

- `incoming-docs/2026-09-15-CE_algorithm_v2.md` §A2–A8, §«Полоса», §«Счёт дома».
