# gRPC Method: OrderFlowService/CreateFlowOrder

## Status

draft (proto-defined)

## Purpose

Основная синхронная точка входа Gateway для создания FlowOrder. Принимает заявку с полным CSLO-параметризированием (диапазон цены, max speed, total qty), выполняет pre-trade risk-проверку, резервирует средства в Ledger и публикует нормализованную заявку в Kafka `orders.normalized`.

## Transport

gRPC

## Service

`fob.orders.v1.OrderFlowService`

## Method

```proto
rpc CreateFlowOrder(CreateFlowOrderRequest) returns (CreateFlowOrderResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST → gRPC translation)
- [risk-manager](../../05-components/risk-manager/overview.md) — для internal liquidation orders (F-08)

## Callee

- [order-flow](../../05-components/order-flow/overview.md)

## Schema

См. [contracts/proto/fob/orders/v1/order_flow_service.proto](../../../contracts/proto/fob/orders/v1/order_flow_service.proto).

```proto
message CreateFlowOrderRequest {
  fob.common.v1.EventMeta meta = 1;
  FlowOrder order = 2;
}

message CreateFlowOrderResponse {
  fob.common.v1.EventMeta meta = 1;
  bool accepted = 2;
  string order_id = 3;
  fob.common.v1.Error error = 4;
}
```

## Idempotency

По `client_order_id` (= `meta.correlation_id` в текущем MVP). Повторный вызов с тем же ключом возвращает существующий `order_id`.

## Used In Features

- [F-02. Create FlowOrder](../../02-system/features/F-02-create-floworder/)
- [F-08. Liquidations](../../02-system/features/F-08-posttrade-risk-and-liquidations/) (internal call)

## Used In Use Cases

- [UC-F02-01. Create FlowOrder](../../02-system/use-cases/UC-F02-01-create-flow-order/use-case.md)
- [UC-F08-01. Liquidate Position](../../02-system/use-cases/UC-F08-01-liquidate-position/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F02-UC-F02-01-services](../../05-components/sequences/SEQ-F02-UC-F02-01-services.md)
- [SEQ-F08-UC-F08-01-services](../../05-components/sequences/SEQ-F08-UC-F08-01-services.md)

## Related Components

- [gateway](../../05-components/gateway/overview.md)
- [order-flow](../../05-components/order-flow/overview.md)
- [risk-manager](../../05-components/risk-manager/overview.md)
- [ledger](../../05-components/ledger/overview.md)

## Related Data Objects

- [`flow_orders`](../../07-data/oltp-schema.md#таблица-flow_orders) (создание строки)
- Kafka `orders.normalized` (производимое событие)

## Source Fragments

- IN-001-FR-016 (FR-ORD-001 create)
- IN-001-FR-022 (компонент OrderFlow)
- IN-001-FR-023 (sequence hint)
