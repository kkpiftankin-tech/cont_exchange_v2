# gRPC Method: LedgerService/GetBalances

## Status

draft (proto-defined)

## Purpose

Получить текущие балансы пользователя — список `Balance{currency, available, reserved, total}` по всем валютам аккаунта. Используется UI/Gateway, Risk Manager (для pre-trade snapshot) и operator-консолью.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc GetBalances(GetBalancesRequest) returns (GetBalancesResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `GET /v1/balances` → gRPC)
- [risk-manager](../../05-components/risk-manager/overview.md) (опциональный snapshot для `CheckNewOrder`)

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

См. [contracts/proto/fob/ledger/v1/ledger.proto](../../../contracts/proto/fob/ledger/v1/ledger.proto).

```proto
message GetBalancesRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
}

message Balance {
  string currency = 1;
  fob.common.v1.Decimal available = 2;
  fob.common.v1.Decimal reserved = 3;
  fob.common.v1.Decimal total = 4;
}

message GetBalancesResponse {
  fob.common.v1.EventMeta meta = 1;
  repeated Balance balances = 2;
}
```

## Used In Features

- [F-06. Positions / PnL / Margin](../../02-system/features/F-06-positions-pnl-margin/)
- [F-02. Create FlowOrder](../../02-system/features/F-02-create-floworder/) (косвенно через Risk snapshot)
- [F-07. Pre-Trade Risk](../../02-system/features/F-07-pretrade-risk/)

## Used In Use Cases

- [UC-F06-01](../../02-system/use-cases/UC-F06-01-show-positions/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F06-UC-F06-01-services](../../05-components/sequences/SEQ-F06-UC-F06-01-services.md)

## Related Components

- [ledger](../../05-components/ledger/overview.md)
- [gateway](../../05-components/gateway/overview.md)

## Related Data Objects

- `accounts` (OLTP) — источник данных по балансам

## Source Fragments

- IN-001-FR-016 (FR-LDG-001 balances)
- IN-001-FR-022
