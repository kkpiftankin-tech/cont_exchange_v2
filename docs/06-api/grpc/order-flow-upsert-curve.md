# gRPC Method: OrderFlowService/UpsertCurve

## Status

TODO / planned

## Purpose

Маркет-мейкерский интерфейс: опубликовать (или обновить) непрерывную кривую ликвидности «цена–скорость» в виде набора bucket-ов. Кривая транслируется в набор FlowOrder с meta-тэгом `mm=true`, которые автоматически обновляются на каждом batch-цикле.

## Transport

gRPC

## Service

`fob.orders.v1.OrderFlowService` (planned extension)

## Method (planned)

```proto
rpc UpsertCurve(UpsertCurveRequest) returns (UpsertCurveResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `PUT /v1/mm/curves/{symbol}` → gRPC)
- MM-bots/algos через прямой gRPC

## Callee

- [order-flow](../../05-components/order-flow/overview.md)

## Schema

TODO. Предлагаемая форма:

```proto
message CurveBucket {
  fob.common.v1.Decimal price_low = 1;
  fob.common.v1.Decimal price_high = 2;
  fob.common.v1.Decimal max_qty = 3;
  fob.common.v1.Decimal max_speed = 4;
  fob.common.v1.Side side = 5;
}

message UpsertCurveRequest {
  fob.common.v1.EventMeta meta = 1;
  string mm_user_id = 2;
  string symbol = 3;
  repeated CurveBucket buckets = 10;
  google.protobuf.Timestamp valid_until = 11;
}

message UpsertCurveResponse {
  fob.common.v1.EventMeta meta = 1;
  bool accepted = 2;
  string curve_id = 3;
  repeated string order_ids = 4;       // соответствуют bucket-ам
  fob.common.v1.Error error = 5;
}
```

## Idempotency

По `(mm_user_id, symbol)`. Повторный upsert заменяет предыдущую активную кривую (cancel-replace всех её ног).

## Used In Features

- [F-10. MM Curves](../../02-system/features/F-10-mm-curves/)

## Used In Use Cases

- [UC-F10-01. Publish MM Curve](../../02-system/use-cases/UC-F10-01-publish-mm-curve/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F10-UC-F10-01-services](../../05-components/sequences/SEQ-F10-UC-F10-01-services.md)

## Related Components

- [order-flow](../../05-components/order-flow/overview.md)
- [matching-fob-core](../../05-components/matching-fob-core/overview.md)

## Related Data Objects

- [`flow_orders`](../../07-data/oltp-schema.md#таблица-flow_orders) + `mm` тэг + `curve_id`

## Source Fragments

- IN-001-FR-016 (FR-MM-001 curve)
- IN-001-FR-022
