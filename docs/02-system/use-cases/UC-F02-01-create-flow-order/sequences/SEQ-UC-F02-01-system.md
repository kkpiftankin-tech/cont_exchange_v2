# SEQ-UC-F02-01-system. Create FlowOrder: system view

## Type

System Context Sequence

## Feature

- [F-02](../../../features/F-02-create-floworder/)

## Use Case

- [UC-F02-01](../use-case.md)

## Purpose

Trader взаимодействует с Continuous Exchange System как с черным ящиком: задаёт параметры FlowOrder, получает preview, подтверждает, получает статус и поток обновлений.

## Participants

- Trader
- Continuous Exchange System

## Diagram

```mermaid
sequenceDiagram
    actor T as Trader
    participant S as Continuous Exchange System

    T->>S: Ввод параметров Flow Order
    S-->>T: Preview VWAP / IS / risk estimate

    T->>S: Confirm Flow Order
    S-->>T: Order accepted / rejected / throttled

    S-->>T: Order status active
    S-->>T: Execution updates (fills)
```

## Related Service Sequence

- [SEQ-F02-UC-F02-01-services](../../../../05-components/sequences/SEQ-F02-UC-F02-01-services.md)

## Related Contracts

- `POST /v1/flow-orders` — [../../../../06-api/rest/](../../../../06-api/rest/)
- WebSocket `OrderStatusUpdate` (планируется)
