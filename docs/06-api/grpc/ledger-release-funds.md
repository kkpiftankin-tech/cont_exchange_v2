# gRPC Method: LedgerService/ReleaseFunds

## Status

draft (proto-defined)

## Purpose

Освободить ранее зарезервированные средства — при отмене заявки, при reject от Risk Manager, при истечении TIF. Идемпотентная операция по `reservation_id`.

В сервисных диаграммах F-03 упоминается как `ReleaseReservation` — это синоним; канонический proto-метод — `ReleaseFunds`.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc ReleaseFunds(ReleaseFundsRequest) returns (google.protobuf.Empty);
```

## Caller

- [order-flow](../../05-components/order-flow/overview.md) (cancel / TIF expire)
- [risk-manager](../../05-components/risk-manager/overview.md) (reject post-trade resize)

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

См. [contracts/proto/fob/ledger/v1/ledger.proto](../../../contracts/proto/fob/ledger/v1/ledger.proto).

```proto
message ReleaseFundsRequest {
  fob.common.v1.EventMeta meta = 1;
  string reservation_id = 2;
  string user_id = 3;
  string order_id = 4;
  string currency = 10;
  fob.common.v1.Decimal amount = 11;
  string reason = 12;
}
```

## Idempotency

По `reservation_id`. Повторный release того же резерва не уменьшает баланс повторно.

## Used In Features

- [F-03. Order Lifecycle](../../02-system/features/F-03-order-lifecycle/)
- [F-02. Create FlowOrder](../../02-system/features/F-02-create-floworder/) (reject path)

## Used In Use Cases

- [UC-F03-01](../../02-system/use-cases/UC-F03-01-amend-cancel-order/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F03-UC-F03-01-services](../../05-components/sequences/SEQ-F03-UC-F03-01-services.md)

## Related Components

- [order-flow](../../05-components/order-flow/overview.md)
- [ledger](../../05-components/ledger/overview.md)

## Related Data Objects

- `accounts.reserved` (OLTP) — уменьшается на `amount`

## Source Fragments

- IN-001-FR-016 (FR-LDG-003 release)
- IN-001-FR-022
