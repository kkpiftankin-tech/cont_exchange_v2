# gRPC Method: LedgerService/ReserveFunds

## Status

draft (proto-defined)

## Purpose

Зарезервировать средства пользователя под новую или изменённую заявку. Идемпотентная операция по `reservation_id` (обычно = `client_order_id`). При недостатке свободных средств возвращает `success=false` с детализированной ошибкой.

В сервисных диаграммах часто упоминается как `Reserve` — это сокращение от `ReserveFunds`.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc ReserveFunds(ReserveFundsRequest) returns (ReserveFundsResponse);
```

## Caller

- [order-flow](../../05-components/order-flow/overview.md) — после успешного `RiskService/CheckNewOrder`

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

См. [contracts/proto/fob/ledger/v1/ledger.proto](../../../contracts/proto/fob/ledger/v1/ledger.proto).

```proto
enum ReserveReason {
  RESERVE_REASON_UNSPECIFIED = 0;
  RESERVE_REASON_NEW_ORDER = 1;
  RESERVE_REASON_AMEND_ORDER = 2;
  RESERVE_REASON_HEDGE = 3;
}

message ReserveFundsRequest {
  fob.common.v1.EventMeta meta = 1;
  string reservation_id = 2;     // idempotency key
  string user_id = 3;
  string order_id = 4;
  string currency = 10;
  fob.common.v1.Decimal amount = 11;
  ReserveReason reason = 12;
}

message ReserveFundsResponse {
  fob.common.v1.EventMeta meta = 1;
  bool success = 2;
  fob.common.v1.Error error = 3;
  fob.common.v1.Decimal reserved_amount = 10;
}
```

## Idempotency

По `reservation_id`. Повтор того же запроса возвращает существующий результат, не двоит резерв.

## Used In Features

- [F-02. Create FlowOrder](../../02-system/features/F-02-create-floworder/)

## Used In Use Cases

- [UC-F02-01](../../02-system/use-cases/UC-F02-01-create-flow-order/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F02-UC-F02-01-services](../../05-components/sequences/SEQ-F02-UC-F02-01-services.md)

## Related Components

- [order-flow](../../05-components/order-flow/overview.md)
- [ledger](../../05-components/ledger/overview.md)

## Related Data Objects

- `accounts.reserved` (OLTP) — увеличивается на `amount`
- audit trail reservations

## Source Fragments

- IN-001-FR-016 (FR-LDG-002 reserve)
- IN-001-FR-022, IN-001-FR-023
