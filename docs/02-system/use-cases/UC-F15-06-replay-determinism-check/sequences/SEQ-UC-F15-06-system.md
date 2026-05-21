# SEQ-UC-F15-06-system. Determinism Check: system view

## Type

System Context Sequence

## Feature

- [F-15](../../../features/F-15-backtest-replay/)

## Use Case

- [UC-F15-06](../use-case.md)

## Diagram

```mermaid
sequenceDiagram
    actor Q as QA
    participant S as Continuous Exchange System

    Q->>S: POST /api/v1/replay/determinism-checks {sessionA, sessionB}
    S->>S: RBAC + ownership
    S->>S: Validate snapshot equality (configs, strategy, date_range, seed)
    S->>S: Load AgentLog of both
    S->>S: Per-batch compare (state, action, reward, pnl, is, fillrate, residual)
    S->>S: Compare summaries
    S-->>Q: 200 OK {deterministic, mismatches}
```
