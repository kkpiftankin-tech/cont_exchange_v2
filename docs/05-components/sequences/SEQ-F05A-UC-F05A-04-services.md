<!--
---
id: SEQ-F05A-UC-F05A-04-services
level: sea
---
-->

# SEQ-F05A-UC-F05A-04-services. Validate on Real Order Books: service view

## Type

Service Interaction Sequence (L1 🌊)

## Feature

- [F-05A. Vectorized External Liquidity](../../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Use Case

- [UC-F05A-04. Validate Algorithm on Real Exchange Order Books](../../02-system/use-cases/UC-F05A-04-validate-real-orderbooks/use-case.md)

## Purpose

Детализирует offline validation: fixture store → market-data vectorization →
matching solver → сравнение с expected. Без live API, без Kafka (in-process harness).

## Participants

- Test harness / CI
- Fixture store (`tests/fixtures/real-orderbooks/`)
- market-data (vectorization)
- matching (`vector_qp_solver`)
- ClickHouse (опц., integration)

## Diagram

```mermaid
sequenceDiagram
    participant T as Test harness / CI
    participant FX as Fixture store
    participant MDS as market-data
    participant MB as matching
    participant CH as ClickHouse

    T->>FX: load fixture(s) + verify sha256 (quality gate)
    FX-->>T: raw order books + metadata
    T->>MDS: vectorize(raw levels)
    MDS-->>T: VectorFlowSegment[] / W
    T->>MB: solveVectorClearing(W, pH, D, q)
    MB-->>T: x, pi, residual, diagnostics
    T->>T: assert residualNorm < tol OR explicit surplus; assert source mapping
    opt integration
        MB->>CH: vector_clearing_results
    end
```

## Related Contracts

- `contracts/proto/fob/marketdata/v1/vector_liquidity.proto` (planned); venue APIs — только refresh fixtures.

## Related Components

- market-data, matching, test harness

## Related Data

- `tests/fixtures/real-orderbooks/`; CH `vector_clearing_results` (integration)

## Contract binding

| Arrow | Transport | Contract |
| --- | --- | --- |
| harness → fixture store | file (immutable + sha256) | `tests/fixtures/real-orderbooks/*` |
| harness → market-data | in-process | vectorize (`VectorFlowSegment[]`) |
| harness → matching | in-process / gRPC | `solveVectorClearing` (`VectorClearingResult`) |
| matching → ClickHouse | SQL | `vector_clearing_results` (planned) |
