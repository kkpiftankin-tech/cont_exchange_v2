# SEQ-UC-F08-01-system. Liquidation: system view

## Type

System Context Sequence

## Feature

- [F-08](../../../features/F-08-posttrade-risk-and-liquidations/)

## Use Case

- [UC-F08-01](../use-case.md)

## Participants

- Trader
- Continuous Exchange System
- Operator

## Diagram

```mermaid
sequenceDiagram
    participant S as Continuous Exchange System
    actor T as Trader
    actor O as Operator

    S->>S: post-trade risk check (margin breach)
    S-->>T: margin call notification
    S->>S: create liquidation order
    S-->>T: liquidation in progress
    S-->>O: liquidation event
    S-->>T: position closed
```

## Related Service Sequence

- [SEQ-F08-UC-F08-01-services](../../../../05-components/sequences/SEQ-F08-UC-F08-01-services.md)
