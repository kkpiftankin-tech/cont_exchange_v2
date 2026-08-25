# gRPC Method: OrderFlowService/CreateBatchOrder

## Status

TODO / planned — описан в [order-flow-create-combo-order.md](order-flow-create-combo-order.md).
Отдельный файл как точка трассировки.

## Purpose

Создать `BatchOrder` — клиентский parent object, объединяющий несколько дочерних
`ComboOrder`, условных ветвей или одиночных `FlowOrder`. Используется для:

- TWAP/VWAP containers;
- смешанных групп (combo + conditional + одиночные ноги);
- групповой отмены и единого статуса.

## Transport

gRPC

## Service

`fob.orders.v1.OrderFlowService`

## Method (planned)

```proto
rpc CreateBatchOrder(CreateBatchOrderRequest) returns (CreateBatchOrderResponse);
```

## Schema

Полная схема — в [order-flow-create-combo-order.md](order-flow-create-combo-order.md#createbatchorder).

## Idempotency

По `client_batch_id`.

## Used In Features

- [F-09](../../02-system/features/F-09-batch-combo-orders/)

## Source Fragments

- IN-011 §4.1, §8.1, §11.1
```

---
