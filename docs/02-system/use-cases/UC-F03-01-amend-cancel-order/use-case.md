# UC-F03-01. Изменить или отменить FlowOrder

## Feature

- [F-03. Order Lifecycle](../../features/F-03-order-lifecycle/)

## Primary Actor

Trader

## Preconditions

- FlowOrder существует и находится в статусе `active`.
- Trader — владелец заявки.

## Trigger

Trader посылает `AmendFlowOrder` или `CancelFlowOrder`.

## Main Flow

1. Trader отправляет команду amend/cancel.
2. Order Flow валидирует.
3. Risk Manager переоценивает (для amend).
4. Ledger обновляет резерв (если изменился `q_max`).
5. Order Flow публикует `orders.normalized` с типом AMEND/CANCEL.
6. Matching обновляет состояние active orders.
7. Возвращается обновлённый статус.

## Alternative Flows

### A1. Order уже исполнен

1. Order Flow возвращает `INVALID_STATE`.

### A2. Amend нарушает risk-limits

1. Risk Manager возвращает `REJECT`. Команда отвергнута.

## Postconditions

- `flow_orders.status` обновлён.
- Резерв скорректирован.
- Событие в `orders.normalized`.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F03-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F03-UC-F03-01-services.md)

## Related Contracts

- `AmendFlowOrderRequest` / `CancelFlowOrderRequest` — [../../../06-api/grpc/](../../../06-api/grpc/)
- [orders.normalized](../../../06-api/messaging/orders-normalized.md)

## Related Components

- [gateway](../../../05-components/gateway/overview.md)
- [order-flow](../../../05-components/order-flow/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [ledger](../../../05-components/ledger/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)

## Related Data

- [flow_orders](../../../07-data/data-overview.md)
