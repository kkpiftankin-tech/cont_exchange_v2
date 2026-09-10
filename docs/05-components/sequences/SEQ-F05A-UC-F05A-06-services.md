<!--
---
id: SEQ-F05A-UC-F05A-06-services
level: sea
---
-->

# SEQ-F05A-UC-F05A-06-services. Run CE Tact with Virtual Counterparties: service view

## Type

Service Interaction Sequence (L1 🌊)

## Feature

- [F-05A. Vectorized External Liquidity](../../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Use Case

- [UC-F05A-06. Run CE Tact with Virtual Counterparties](../../02-system/use-cases/UC-F05A-06-ce-agent-tact-clearing/use-case.md)

## Purpose

Детализирует такт CE: `market-data` (оконный агрегатор + `agent_builder`) → `matching`
(граф узлов, QP, инварианты, потоки агентов, `committed`) → `ledger` (узловые остатки) →
`risk` (`Z_a`, форматтер заявок) → `venues` (исполнение) → `execution.reports` (пересчёт
факта). Трасса такта уходит в `clearing_trace`.

## Participants

- market-data (`agent_builder`, оконный агрегатор)
- matching (node vector solver, инварианты, `committed`)
- Kafka (`marketdata.vectorized`, `ce.position.delta`, `matching.clearing_trace`, `execution.intents`, `execution.reports`, `risk.alerts`)
- ledger (узловые остатки `asset@venue`)
- risk-manager (`Z_a`, форматтер заявок)
- venues (External Venues Connector)
- ClickHouse (`clearing_trace`, `node_balances_history`)
- PostgreSQL (`agent_state`, `node_balances`, `f05a_clearing_config`)

## Diagram

```mermaid
sequenceDiagram
    participant MDS as market-data
    participant K as Kafka
    participant MB as matching
    participant PG as PostgreSQL
    participant CH as ClickHouse
    participant L as ledger
    participant R as risk-manager
    participant V as venues

    MDS->>MDS: window aggregate books; build agents (translator+link)
    MDS->>K: VectorClearingInput (node basis, agent segments) -> marketdata.vectorized
    K-->>MB: consume VectorClearingInput
    MB->>PG: read agent_state.committed, f05a_clearing_config
    MB->>L: GetNodeBalances (Q_{a,venue}, target, in_transit)
    MB->>MB: clear node graph (Wx=0 venue nodes, inventory box book nodes); flows f
    MB->>MB: check invariants (I-T1..I-T6)
    alt invariants pass
        MB->>PG: committed += |f|  (txn with emit)
        MB->>K: ce.position.delta -> ledger
        MB->>K: matching.clearing_trace ; MB->>CH: clearing_trace
        K-->>L: ce.position.delta -> ApplyPositionDelta (node double-entry)
        R->>L: GetNodeBalances / Z_a
        R->>K: hedge orders -> execution.intents (venue from agent)
        K-->>V: execution.intents
        V->>K: execution.reports
        K-->>L: execution.reports -> update Q_{a,venue}, node_balances_history
        K-->>MB: execution.reports -> committed -= filled
    else invariant failed
        MB->>K: risk.alerts (failed tact) ; MB->>CH: clearing_trace (failed)
    end
```

## Related Contracts

- `contracts/proto/fob/agent/v1/agent.proto` (planned), `vector_liquidity.proto` (`NodeBasis`, planned)
- `contracts/proto/fob/ledger/v1/ledger.proto` (`GetNodeBalances`, planned), `risk.proto` (`Z_a`, planned)
- Kafka `marketdata.vectorized`, `ce.position.delta`, `matching.clearing_trace` (planned), `execution.intents`, `execution.reports`, `risk.alerts`

## Related Components

- market-data, matching, ledger, risk-manager, venues

## Related Data

- PG `agent_state`, `node_balances`, `f05a_clearing_config`
- CH `clearing_trace`, `node_balances_history` (planned), `vector_flow_segments_history` (+9 cols)

## Related ADR

- [ADR-055](../../03-architecture/adr/ADR-055-ce-virtual-counterparties-agents.md), [ADR-056](../../03-architecture/adr/ADR-056-ce-node-graph-clearing.md), [ADR-057](../../03-architecture/adr/ADR-057-ce-three-level-position.md), [ADR-058](../../03-architecture/adr/ADR-058-ce-committed-inflight-state.md), [ADR-059](../../03-architecture/adr/ADR-059-ce-clearing-trace-audit.md)

## Contract binding

| Arrow | Transport | Contract |
| --- | --- | --- |
| market-data → matching | Kafka | `marketdata.vectorized` (`VectorClearingInput` + node basis + agent segments) |
| matching → PostgreSQL | SQL | `agent_state` (committed), `f05a_clearing_config` |
| matching → ledger | gRPC | `GetNodeBalances` (planned) |
| matching → ledger | Kafka | `ce.position.delta` (`CePositionDeltaBatch`) |
| matching → Kafka/CH | Kafka/SQL | `matching.clearing_trace` (`ClearingTrace`, planned) |
| risk → Kafka | Kafka | `execution.intents` (`ExecutionIntent`, venue from agent) |
| venues → Kafka | Kafka | `execution.reports` (`ExecutionReport`) |
| matching → risk | Kafka | `risk.alerts` (`RiskAlert`, failed invariant) |

## Data binding

| Node | Store | Object |
| --- | --- | --- |
| agent committed/assigned | PostgreSQL | `agent_state(agent_id, committed, assigned_last, updated_at)` |
| node balances | PostgreSQL | `node_balances(account, asset, venue, qty, target, in_transit)` |
| tact config | PostgreSQL | `f05a_clearing_config(+theta, dhl_fraction, rho, z_limit, gamma)` |
| tact trace | ClickHouse | `clearing_trace` (agents/nodes/invariants/pnl/orders JSON) |
| node balance history | ClickHouse | `node_balances_history(asset, venue, qty, target, in_transit, event_time_ms)` |
| segment diagnostics | ClickHouse | `vector_flow_segments_history` (+9 cols: seg_index, anchor, slope, q_min, alpha_ext, alpha_t, beta_t, theta, translator_model) |
