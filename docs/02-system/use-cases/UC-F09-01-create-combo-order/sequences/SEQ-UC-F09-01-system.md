# SEQ-UC-F09-01-system. Combo Order: system view

## Type

System Context Sequence

## Feature

- [F-09](../../../features/F-09-batch-combo-orders/)

## Use Case

- [UC-F09-01](../use-case.md)

## Participants

- Trader
- Continuous Exchange System

## Diagram

```mermaid
sequenceDiagram
    actor T as Trader
    participant S as Continuous Exchange System

    T->>S: CreateComboOrder(legs)
    S-->>T: combo accepted / rejected (all-or-nothing)
    S-->>T: per-leg status updates
```

## Related Service Sequence

- [SEQ-F09-UC-F09-01-services](../../../../05-components/sequences/SEQ-F09-UC-F09-01-services.md)
