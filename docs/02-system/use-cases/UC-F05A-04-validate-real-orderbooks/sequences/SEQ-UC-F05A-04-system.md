<!--
---
id: SEQ-UC-F05A-04-system
title: "Validate on Real Order Books: system view"
level: kite
parent-uc: UC-F05A-04
---
-->

# SEQ-UC-F05A-04-system. Validate on Real Order Books: system view

## Type

System Context Sequence (L0 ☁️)

## Feature

- [F-05A. Vectorized External Liquidity](../../../features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Use Case

- [UC-F05A-04](../use-case.md)

## Purpose

Показать validation-цикл на реальных captured стаканах (offline, детерминированно).

## Participants

- Researcher / Developer
- Continuous Exchange System

## Diagram

```mermaid
sequenceDiagram
    actor R as Researcher / Developer
    participant S as Continuous Exchange System

    R->>S: Select captured fixture(s) (>= 2 venues)
    S->>S: Load raw order books -> vectorize -> W
    S->>S: Solve -> x, residual
    S-->>R: W, x, Wx, residualNorm (expected vs actual)
    Note over R,S: offline, no live API; fixtures immutable + sha256
```

## Related Service Sequence

- [../../../../05-components/sequences/SEQ-F05A-UC-F05A-04-services.md](../../../../05-components/sequences/SEQ-F05A-UC-F05A-04-services.md)

## Related Contracts

- Venue order book APIs (Binance/Coinbase/Kraken) — только для refresh fixtures.
