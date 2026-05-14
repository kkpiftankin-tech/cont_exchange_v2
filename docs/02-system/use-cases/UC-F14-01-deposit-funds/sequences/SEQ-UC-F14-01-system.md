# SEQ-UC-F14-01-system. Deposit: system view

## Type

System Context Sequence

## Feature

- [F-14](../../../features/F-14-deposit-withdraw/)

## Use Case

- [UC-F14-01](../use-case.md)

## Participants

- Trader
- Continuous Exchange System
- Custody / Payment Provider

## Diagram

```mermaid
sequenceDiagram
    actor T as Trader
    participant S as Continuous Exchange System
    participant C as Custody Provider

    T->>S: initiate deposit
    S->>C: request deposit address
    C-->>S: address / instructions
    S-->>T: address / instructions
    T->>C: send funds
    C-->>S: confirmation
    S-->>T: balance updated
```

## Related Service Sequence

- [SEQ-F14-UC-F14-01-services](../../../../05-components/sequences/SEQ-F14-UC-F14-01-services.md)
