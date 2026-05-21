# SEQ-UC-F15-03-system. A/B Compare Sessions: system view

## Type

System Context Sequence

## Feature

- [F-15](../../../features/F-15-backtest-replay/)

## Use Case

- [UC-F15-03](../use-case.md)

## Diagram

```mermaid
sequenceDiagram
    actor A as Analyst
    participant S as Continuous Exchange System

    A->>S: GET /api/v1/replay/compare?sessionA=&sessionB=
    S->>S: RBAC + ownership
    S->>S: ValidatePair (status, date range)
    alt cache hit
        S-->>A: 200 OK CompareResponse (cached)
    else cache miss
        S->>S: Load both replay_summaries
        S->>S: Compute deltas + direction-aware better
        S->>S: Cache result
        S-->>A: 200 OK CompareResponse
    end
```
