# gRPC Method: OrderFlowService/GetFlowOrder

## Status

draft (proto-defined)

## Purpose

Прочитать текущее состояние FlowOrder по `(user_id, order_id)`. Используется UI/Gateway для отображения статуса, остатка и истории состояний заявки.

## Transport

gRPC

## Service

`fob.orders.v1.OrderFlowService`

## Method

```proto
rpc GetFlowOrder(GetFlowOrderRequest) returns (GetFlowOrderResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `GET /v1/flow-orders/{id}` → gRPC)

## Callee

- [order-flow](../../05-components/order-flow/overview.md)

## Schema

См. [contracts/proto/fob/orders/v1/order_flow_service.proto](../../../contracts/proto/fob/orders/v1/order_flow_service.proto).

```proto
message GetFlowOrderRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
  string order_id = 3;
}

message GetFlowOrderResponse {
  fob.common.v1.EventMeta meta = 1;
  FlowOrderView view = 2;
}
```

## Used In Features

- [F-02. Create FlowOrder](../../02-system/features/F-02-create-floworder/) (read-after-write)
- [F-03. Order Lifecycle](../../02-system/features/F-03-order-lifecycle/)

## Used In Use Cases

- [UC-F02-01](../../02-system/use-cases/UC-F02-01-create-flow-order/use-case.md)
- [UC-F03-01](../../02-system/use-cases/UC-F03-01-amend-cancel-order/use-case.md)

## Related Components

- [gateway](../../05-components/gateway/overview.md)
- [order-flow](../../05-components/order-flow/overview.md)

## Related Data Objects

- [`flow_orders`](../../07-data/oltp-schema.md#таблица-flow_orders) (read)

## Source Fragments

- IN-001-FR-016 (FR-ORD-003 read)
- IN-001-FR-022
