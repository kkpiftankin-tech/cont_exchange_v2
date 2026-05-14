# SEQ-UC-F12-01-system. Execution Hedge: system view

## Type

System Context Sequence

## Feature

- [F-12](../../../features/F-12-execution-hedge/)

## Use Case

- [UC-F12-01](../use-case.md)

## Participants

- Continuous Exchange System
- CEX / DEX

## Diagram

```mermaid
sequenceDiagram
    participant S as Continuous Exchange System
    participant V as CEX / DEX

    S->>S: detect inventory breach
    S->>V: child order (hedge intent)
    V-->>S: ack / fill / reject
    S->>S: apply hedge to ledger
```

## Related Service Sequence

- [SEQ-F12-UC-F12-01-services](../../../../05-components/sequences/SEQ-F12-UC-F12-01-services.md)
