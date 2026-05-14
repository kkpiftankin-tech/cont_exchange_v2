# SEQ-UC-F16-01-system. Kill-Switch: system view

## Type

System Context Sequence

## Feature

- [F-16](../../../features/F-16-operator-console/)

## Use Case

- [UC-F16-01](../use-case.md)

## Participants

- Operator
- Continuous Exchange System
- Trader (получатель уведомления)

## Diagram

```mermaid
sequenceDiagram
    actor O as Operator
    participant S as Continuous Exchange System
    actor T as Trader

    O->>S: ActivateKillSwitch(scope)
    S-->>O: ack + audit id
    S-->>T: trading halted (reason, scope)
    Note over S: all new orders rejected
```

## Related Service Sequence

- [SEQ-F16-UC-F16-01-services](../../../../05-components/sequences/SEQ-F16-UC-F16-01-services.md)
