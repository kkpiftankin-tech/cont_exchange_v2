# gRPC Method: LedgerService/ApplyDeposit

## Status

TODO / planned

## Purpose

Зачислить депозит на user-account после подтверждения custody-adapter-ом. Идемпотентная операция по `external_tx_id` — повторная доставка той же on-chain транзакции не двоит баланс.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService` (planned extension)

## Method (planned)

```proto
rpc ApplyDeposit(ApplyDepositRequest) returns (ApplyDepositResponse);
```

## Caller

- `custody-adapter` (planned — компонент не создан) (planned) — после подтверждения транзакции

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

TODO. Предлагаемая форма:

```proto
message ApplyDepositRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
  string currency = 3;
  fob.common.v1.Decimal amount = 4;
  string external_tx_id = 5;             // idempotency key
  string deposit_address = 6;
  google.protobuf.Timestamp confirmed_at = 7;
  int32 confirmations = 8;
}

message ApplyDepositResponse {
  fob.common.v1.EventMeta meta = 1;
  bool applied = 2;
  fob.common.v1.Error error = 3;
}
```

## Idempotency

По `external_tx_id`. Повторный вызов возвращает `applied=true` без изменения баланса.

## Used In Features

- [F-14. Deposit / Withdraw](../../02-system/features/F-14-deposit-withdraw/)

## Used In Use Cases

- [UC-F14-01. Deposit Funds](../../02-system/use-cases/UC-F14-01-deposit-funds/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F14-UC-F14-01-services](../../05-components/sequences/SEQ-F14-UC-F14-01-services.md)

## Related Components

- [ledger](../../05-components/ledger/overview.md)
- `custody-adapter` (planned — компонент не создан) (planned)

## Related Data Objects

- [`collateral_transfers`](../../07-data/oltp-schema.md#таблица-collateral_transfers)
- `accounts` (OLTP — `available` увеличивается)

## Source Fragments

- IN-001-FR-016 (FR-CUST-001 deposit, derived)
- IN-001-FR-022
