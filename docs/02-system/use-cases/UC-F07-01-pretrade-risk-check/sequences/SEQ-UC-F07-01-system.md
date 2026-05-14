# SEQ-UC-F07-01-system. Pre-trade Risk: system view

## Type

System Context Sequence

## Feature

- [F-07](../../../features/F-07-pretrade-risk/)

## Use Case

- [UC-F07-01](../use-case.md)

## Purpose

Внешне видно как часть Create FlowOrder — система возвращает accept/reject/throttle.

## Participants

- Trader
- Continuous Exchange System

## Diagram

```mermaid
sequenceDiagram
    actor T as Trader
    participant S as Continuous Exchange System

    T->>S: Submit FlowOrder
    S->>S: pre-trade risk check
    alt accepted
        S-->>T: order active
    else throttled
        S-->>T: adjusted q_rate / q_max
    else rejected
        S-->>T: reject reason
    end
```

## Related Service Sequence

- [SEQ-F07-UC-F07-01-services](../../../../05-components/sequences/SEQ-F07-UC-F07-01-services.md)
