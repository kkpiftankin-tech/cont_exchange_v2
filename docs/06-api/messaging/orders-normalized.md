# Kafka Topic: orders.normalized

## Purpose

Нормализованные команды по FlowOrder (CREATE / AMEND / CANCEL) после успешной pre-trade проверки. Является источником для matching и для downstream подписчиков (market-data, observability, backtest).

## Producer

- [order-flow](../../05-components/order-flow/overview.md)
- (planned) [external-venues](../../05-components/external-venues/overview.md) — для FOB curves из внешней ликвидности

## Consumers

- [matching-fob-core](../../05-components/matching-fob-core/overview.md)
- [observability-reporting](../../05-components/observability-reporting/overview.md)
- (planned) backtest-replay
- (planned) WebSocket gateway → UI

## Settings

| Параметр | Значение |
| --- | --- |
| Retention | 7 дней |
| Partition key | `symbol` |
| Delivery | at-least-once, idempotent consumer |
| Schema | `fob.orders.v1.OrdersNormalized` (Protobuf) |

## Message schema

См. [contracts/proto/fob/orders/v1/orders.proto](../../../contracts/proto/fob/orders/v1/orders.proto), сообщение `OrdersNormalized`.

## Used In Features

- [F-02. Create FlowOrder](../../02-system/features/F-02-create-floworder/)
- [F-03. Order Lifecycle](../../02-system/features/F-03-order-lifecycle/)
- [F-09. Combo Orders](../../02-system/features/F-09-batch-combo-orders/)
- [F-10. MM Curves](../../02-system/features/F-10-mm-curves/)
- [F-11. External Venues](../../02-system/features/F-11-external-venues-lob-to-fob/)
- [F-15. Backtest / Replay](../../02-system/features/F-15-backtest-replay/)

## Used In Use Cases

- [UC-F02-01](../../02-system/use-cases/UC-F02-01-create-flow-order/use-case.md)
- [UC-F03-01](../../02-system/use-cases/UC-F03-01-amend-cancel-order/use-case.md)
- [UC-F09-01](../../02-system/use-cases/UC-F09-01-create-combo-order/use-case.md)
- [UC-F10-01](../../02-system/use-cases/UC-F10-01-publish-mm-curve/use-case.md)
- [UC-F11-01](../../02-system/use-cases/UC-F11-01-ingest-external-marketdata/use-case.md)
- [UC-F15-01](../../02-system/use-cases/UC-F15-01-replay-historical-batch/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F02-UC-F02-01-services](../../05-components/sequences/SEQ-F02-UC-F02-01-services.md)
- [SEQ-F03-UC-F03-01-services](../../05-components/sequences/SEQ-F03-UC-F03-01-services.md)
- [SEQ-F04-UC-F04-01-services](../../05-components/sequences/SEQ-F04-UC-F04-01-services.md)
- [SEQ-F09-UC-F09-01-services](../../05-components/sequences/SEQ-F09-UC-F09-01-services.md)
- [SEQ-F10-UC-F10-01-services](../../05-components/sequences/SEQ-F10-UC-F10-01-services.md)
- [SEQ-F11-UC-F11-01-services](../../05-components/sequences/SEQ-F11-UC-F11-01-services.md)
- [SEQ-F15-UC-F15-01-services](../../05-components/sequences/SEQ-F15-UC-F15-01-services.md)
