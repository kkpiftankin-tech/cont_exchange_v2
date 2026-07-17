<!--
---
id: SEQ-UC-F05A-05-system
title: "Replay Vector Clearing: system view"
level: kite
parent-uc: UC-F05A-05
---
-->

# SEQ-UC-F05A-05-system. Replay Vector Clearing: system view

## Type

System Context Sequence (L0 ☁️)

## Feature

- [F-05A. Vectorized External Liquidity](../../../features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Use Case

- [UC-F05A-05](../use-case.md)

## Purpose

Показать детерминированный replay vector clearing на исторических данных (black box).

## Participants

- Operator / Researcher
- Continuous Exchange System

## Diagram

```mermaid
sequenceDiagram
    actor O as Operator / Researcher
    participant S as Continuous Exchange System

    O->>S: Create replay session (captured scenario)
    S->>S: Re-vectorize -> W ; re-solve QP (deterministic)
    S-->>O: (W, x, pi, residual) == original (bit-exact) or divergence flagged
```

## Related Service Sequence

- [../../../../05-components/sequences/SEQ-F05A-UC-F05A-05-services.md](../../../../05-components/sequences/SEQ-F05A-UC-F05A-05-services.md)

## Related Contracts

- `backtest.execution.groups` (replay-isolated).
