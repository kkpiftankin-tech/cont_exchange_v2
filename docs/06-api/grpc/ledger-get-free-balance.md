# gRPC Method: LedgerService/GetFreeBalance

## Status

TODO / planned (в текущем `ledger.proto` отсутствует; функционально близкое к `GetBalances`)

## Purpose

Получить свободный (`available`) баланс пользователя в указанной валюте — компактный read-only метод для pre-trade risk-check без выборки полного списка валют.

Альтернатива `GetBalances` с post-filtering на стороне Risk Manager; добавляется как самостоятельный метод, чтобы оптимизировать hot path risk-check.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService` (planned extension)

## Method (planned)

```proto
rpc GetFreeBalance(GetFreeBalanceRequest) returns (GetFreeBalanceResponse);
```

## Caller

- [risk-manager](../../05-components/risk-manager/overview.md) — внутри `CheckNewOrder`

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

TODO. Предлагаемая форма:

```proto
message GetFreeBalanceRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
  string currency = 3;
}

message GetFreeBalanceResponse {
  fob.common.v1.EventMeta meta = 1;
  fob.common.v1.Decimal available = 2;
  fob.common.v1.Decimal reserved = 3;
  fob.common.v1.Error error = 4;
}
```

## Used In Features

- [F-07. Pre-Trade Risk](../../02-system/features/F-07-pretrade-risk/)

## Used In Use Cases

- [UC-F07-01](../../02-system/use-cases/UC-F07-01-pretrade-risk-check/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F07-UC-F07-01-services](../../05-components/sequences/SEQ-F07-UC-F07-01-services.md)

## Related Components

- [ledger](../../05-components/ledger/overview.md)
- [risk-manager](../../05-components/risk-manager/overview.md)

## Related Data Objects

- `accounts` (OLTP)

## Source Fragments

- IN-001-FR-022 (Risk → Ledger)
