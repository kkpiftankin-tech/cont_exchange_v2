# UC-F10-01. Опубликовать MM-кривую ликвидности

## Feature

- [F-10. Market-Maker Curves](../../features/F-10-mm-curves/)

## Primary Actor

Market Maker

## Preconditions

- MM аутентифицирован.
- Имеется маржа для двусторонней котировки.

## Trigger

MM публикует/обновляет CSLO-кривую (bid / ask buckets).

## Main Flow

1. MM посылает `UpsertCurve(symbol, buckets)`.
2. Order Flow раскладывает curve на серию FlowOrder.
3. Risk проверяет совокупный inventory risk.
4. Matching рассматривает буксеты в каждом batch.
5. Возвращается статус кривой.

## Alternative Flows

### A1. Inventory risk breach

1. Risk возвращает `RESIZE`, MM получает скорректированную кривую.

## Postconditions

- Активная MM-curve видна в matching engine.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F10-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F10-UC-F10-01-services.md)

## Related Contracts

- (планируется) `UpsertCurveRequest`
- [orders.normalized](../../../06-api/messaging/orders-normalized.md)

## Related Components

- [order-flow](../../../05-components/order-flow/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)

## Related Data

- [flow_orders](../../../07-data/data-overview.md) с тегом mm
