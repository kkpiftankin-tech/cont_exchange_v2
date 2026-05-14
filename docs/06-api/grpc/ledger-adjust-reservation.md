# gRPC Method: LedgerService/AdjustReservation

## Status

TODO / planned (в текущем `ledger.proto` отсутствует — есть только `ReserveFunds`/`ReleaseFunds`)

## Purpose

Скорректировать размер существующего резерва вверх или вниз — при amend FlowOrder. Если новый объём больше, проверяется доступность дельты; если меньше, дельта возвращается в `available`. Атомарная операция, идемпотентная по `(reservation_id, version)`.

В текущем MVP корректировка эмулируется парой `Release + Reserve`, но это не атомарно и нежелательно для production.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService` (planned extension)

## Method (planned)

```proto
rpc AdjustReservation(AdjustReservationRequest) returns (AdjustReservationResponse);
```

## Caller

- [order-flow](../../05-components/order-flow/overview.md) (на amend FlowOrder)

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

TODO. Предлагаемая форма:

```proto
message AdjustReservationRequest {
  fob.common.v1.EventMeta meta = 1;
  string reservation_id = 2;            // existing reservation
  string user_id = 3;
  string order_id = 4;
  string currency = 10;
  fob.common.v1.Decimal new_amount = 11;
  int64 version = 12;                   // monotonic, для idempotency
  string reason = 13;
}

message AdjustReservationResponse {
  fob.common.v1.EventMeta meta = 1;
  bool success = 2;
  fob.common.v1.Decimal effective_amount = 3;
  fob.common.v1.Error error = 4;
}
```

## Idempotency

По `(reservation_id, version)`. Повторный вызов с тем же version возвращает существующий результат.

## Used In Features

- [F-03. Order Lifecycle](../../02-system/features/F-03-order-lifecycle/)

## Used In Use Cases

- [UC-F03-01](../../02-system/use-cases/UC-F03-01-amend-cancel-order/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F03-UC-F03-01-services](../../05-components/sequences/SEQ-F03-UC-F03-01-services.md)

## Related Components

- [order-flow](../../05-components/order-flow/overview.md)
- [ledger](../../05-components/ledger/overview.md)

## Related Data Objects

- `accounts.reserved`, `accounts.available` (OLTP)

## Source Fragments

- IN-001-FR-016 (FR-LDG-004 adjust, derived)
- IN-001-FR-022
