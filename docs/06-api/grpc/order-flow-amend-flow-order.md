# gRPC Method: OrderFlowService/AmendFlowOrder

## Status

TODO / planned (в текущем `order_flow_service.proto` отсутствует)

## Purpose

Изменить параметры активной FlowOrder без отмены: уточнить `price_low/price_high`, `max_speed`, `total_qty`, продлить окно. На стороне сервиса выполняется повторный risk-check, корректировка резерва в Ledger (`AdjustReservation`) и публикация amend-события в Kafka `orders.normalized`.

## Transport

gRPC

## Service

`fob.orders.v1.OrderFlowService` (planned extension)

## Method (planned)

```proto
rpc AmendFlowOrder(AmendFlowOrderRequest) returns (AmendFlowOrderResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `PATCH /v1/flow-orders/{id}` → gRPC)

## Callee

- [order-flow](../../05-components/order-flow/overview.md)

## Schema

TODO. Предлагаемая форма:

```proto
message AmendFlowOrderRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
  string order_id = 3;
  // optional patches
  fob.common.v1.Decimal new_price_low = 10;
  fob.common.v1.Decimal new_price_high = 11;
  fob.common.v1.Decimal new_max_speed = 12;
  fob.common.v1.Decimal new_total_qty = 13;
  google.protobuf.Timestamp new_window_end = 14;
}

message AmendFlowOrderResponse {
  fob.common.v1.EventMeta meta = 1;
  bool accepted = 2;
  fob.orders.v1.FlowOrder updated = 3;
  fob.common.v1.Error error = 4;
}
```

## Idempotency

По `(order_id, version)` где `version` мономотонно растёт. Повторный amend с тем же version игнорируется.

## Used In Features

- [F-03. Order Lifecycle](../../02-system/features/F-03-order-lifecycle/)

## Used In Use Cases

- [UC-F03-01. Amend/Cancel FlowOrder](../../02-system/use-cases/UC-F03-01-amend-cancel-order/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F03-UC-F03-01-services](../../05-components/sequences/SEQ-F03-UC-F03-01-services.md)

## Related Components

- [order-flow](../../05-components/order-flow/overview.md)
- [risk-manager](../../05-components/risk-manager/overview.md) (re-check)
- [ledger](../../05-components/ledger/overview.md) (adjust reservation)

## Related Data Objects

- [`flow_orders`](../../07-data/oltp-schema.md#таблица-flow_orders) (update)
- Kafka `orders.normalized` (amend event)

## Source Fragments

- IN-001-FR-016 (FR-ORD-004 amend)
- IN-001-FR-022, IN-001-FR-023
