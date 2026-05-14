# gRPC Method: OrderFlowService/CancelFlowOrder

## Status

draft (proto-defined)

## Purpose

Отменить активную FlowOrder. Идемпотентная операция: повторный cancel того же `order_id` возвращает `success=true` без побочных эффектов. На стороне order-flow выполняется освобождение резерва в Ledger и публикация cancel-события в Kafka `orders.normalized`.

## Transport

gRPC

## Service

`fob.orders.v1.OrderFlowService`

## Method

```proto
rpc CancelFlowOrder(CancelFlowOrderRequest) returns (CancelFlowOrderResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `DELETE /v1/flow-orders/{id}` → gRPC)

## Callee

- [order-flow](../../05-components/order-flow/overview.md)

## Schema

См. [contracts/proto/fob/orders/v1/order_flow_service.proto](../../../contracts/proto/fob/orders/v1/order_flow_service.proto).

```proto
message CancelFlowOrderRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
  string order_id = 3;
  string reason = 4;
}

message CancelFlowOrderResponse {
  fob.common.v1.EventMeta meta = 1;
  bool success = 2;
  fob.common.v1.Error error = 3;
}
```

## Idempotency

По `order_id`. Cancel уже отменённой/исполненной заявки возвращает `success=true` без изменения состояния.

## Used In Features

- [F-03. Order Lifecycle (amend/cancel)](../../02-system/features/F-03-order-lifecycle/)

## Used In Use Cases

- [UC-F03-01. Amend/Cancel FlowOrder](../../02-system/use-cases/UC-F03-01-amend-cancel-order/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F03-UC-F03-01-services](../../05-components/sequences/SEQ-F03-UC-F03-01-services.md)

## Related Components

- [gateway](../../05-components/gateway/overview.md)
- [order-flow](../../05-components/order-flow/overview.md)
- [ledger](../../05-components/ledger/overview.md) (release reservation)

## Related Data Objects

- [`flow_orders`](../../07-data/oltp-schema.md#таблица-flow_orders) (status update)
- Kafka `orders.normalized` (cancel event)

## Source Fragments

- IN-001-FR-016 (FR-ORD-002 cancel)
- IN-001-FR-022, IN-001-FR-023
