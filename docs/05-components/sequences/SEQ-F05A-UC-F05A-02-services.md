<!--
---
id: SEQ-F05A-UC-F05A-02-services
level: sea
---
-->

# SEQ-F05A-UC-F05A-02-services. Run Vector Clearing: service view

## Type

Service Interaction Sequence (L1 🌊)

## Feature

- [F-05A. Vectorized External Liquidity](../../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Use Case

- [UC-F05A-02. Run Vector Clearing with F-09 Solver](../../02-system/use-cases/UC-F05A-02-vector-clearing/use-case.md)

## Purpose

Детализирует QP vector clearing: `market-data` → `matching` (OSQP solve, residual,
surplus policy) → `execution.groups` / `fills` → `ledger` (balanced apply) → `ClickHouse`.
Показывает ветки balanced / surplus / failed.

## Participants

- market-data
- matching (`vector_qp_solver`, OSQP)
- Kafka (`marketdata.vectorized`, `execution.groups`, `fills`, `batch.outputs`, `risk.alerts`)
- ledger
- risk-manager
- ClickHouse (`vector_clearing_results`, `surplus_events`)

## Diagram

```mermaid
sequenceDiagram
    participant MDS as market-data
    participant K as Kafka
    participant MB as matching
    participant CH as ClickHouse
    participant L as ledger
    participant R as risk-manager

    MDS->>K: VectorClearingInput -> marketdata.vectorized
    K-->>MB: consume VectorClearingInput (W, pH, D, q)
    MB->>MB: OSQP solve (Wx=0, 0<=x<=q), residual r=Wx
    alt residualNorm < tolerance
        MB->>K: ExecutionGroup -> execution.groups
        MB->>K: FillEvent[] -> fills ; BatchResult -> batch.outputs
        MB->>CH: vector_clearing_results
        K-->>L: ExecutionGroup / FillEvent
        L->>L: balanced apply (Wx~0, no phantom inventory)
    else surplus (residual > tolerance)
        MB->>MB: apply surplus_policy (ADR-047)
        MB->>CH: surplus_events
        MB->>K: SurplusEvent / degraded status
        K-->>R: risk.alerts (surplus above threshold)
        Note over MB,L: EXCHANGE_PNL -> house-account posting (idempotent); REJECT -> no apply
    end
```

## Related Contracts

- [execution.groups](../../06-api/messaging/execution-groups.md), `fills`, `batch.outputs`, `risk.alerts`
- `contracts/proto/fob/marketdata/v1/vector_liquidity.proto` (planned)

## Related Components

- matching, ledger, risk-manager, market-data

## Related Data

- CH `vector_clearing_results`, `surplus_events` (planned)

## Related ADR

- [ADR-048 QP solver backend](../../03-architecture/adr/ADR-048-qp-solver-backend.md), [ADR-047 surplus policy](../../03-architecture/adr/ADR-047-surplus-exchange-pnl-policy.md)

## Contract binding

| Arrow | Transport | Contract |
| --- | --- | --- |
| market-data → matching | Kafka | `marketdata.vectorized` (planned) |
| matching → Kafka | Kafka | `execution.groups` (`ExecutionGroup`), `fills` (`FillEvent`), `batch.outputs` (`BatchResult`) |
| matching → ClickHouse | SQL/HTTP | `vector_clearing_results`, `surplus_events` (planned) |
| matching → risk | Kafka | `risk.alerts` (`RiskAlert`) |
