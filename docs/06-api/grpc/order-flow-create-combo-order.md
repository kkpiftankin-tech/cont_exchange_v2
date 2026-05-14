# gRPC Method: OrderFlowService/CreateComboOrder

## Status

TODO / planned

## Purpose

Создать составную (combo/portfolio) заявку: атомарный набор FlowOrder-ног, который исполняется как единое целое — либо все ноги активны и матчатся одним batch, либо вся combo отменяется. Используется для пар (BTC vs ETH), портфельных стратегий и кросс-инструментных hedge.

## Transport

gRPC

## Service

`fob.orders.v1.OrderFlowService` (planned extension)

## Method (planned)

```proto
rpc CreateComboOrder(CreateComboOrderRequest) returns (CreateComboOrderResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `POST /v1/combo-orders` → gRPC)

## Callee

- [order-flow](../../05-components/order-flow/overview.md)

## Schema

TODO. Предлагаемая форма:

```proto
message ComboLeg {
  fob.orders.v1.FlowOrder order = 1;
  // ratio к "лидеру" combo, например +1.0/-15.0 для BTC/ETH-пары
  fob.common.v1.Decimal ratio = 2;
}

message CreateComboOrderRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
  string client_combo_id = 3;          // idempotency key
  repeated ComboLeg legs = 10;
}

message CreateComboOrderResponse {
  fob.common.v1.EventMeta meta = 1;
  bool accepted = 2;
  string combo_id = 3;
  repeated string order_ids = 4;       // соответствуют legs
  fob.common.v1.Error error = 5;
}
```

## Idempotency

По `client_combo_id`. Все ноги создаются атомарно или ни одна.

## Used In Features

- [F-09. Batch / Combo Orders](../../02-system/features/F-09-batch-combo-orders/)

## Used In Use Cases

- [UC-F09-01. Create Combo Order](../../02-system/use-cases/UC-F09-01-create-combo-order/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F09-UC-F09-01-services](../../05-components/sequences/SEQ-F09-UC-F09-01-services.md)

## Related Components

- [order-flow](../../05-components/order-flow/overview.md)
- [matching-fob-core](../../05-components/matching-fob-core/overview.md) (атомарность combo в solver)

## Related Data Objects

- [`flow_orders`](../../07-data/oltp-schema.md#таблица-flow_orders) + `combo_id` тэг

## Source Fragments

- IN-001-FR-016 (FR-ORD-005 combo)
- IN-001-FR-022
