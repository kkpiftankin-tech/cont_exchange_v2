# gRPC Method: LedgerService/GetPositions

## Status

TODO / planned (в текущем `ledger.proto` отсутствует)

## Purpose

Получить текущие позиции пользователя по всем инструментам: `(symbol, qty, avg_entry_price, unrealized_pnl, realized_pnl)`. Используется UI/Gateway и Risk Manager.

В текущем MVP позиции выводятся косвенно из applied fills; целевая модель — отдельная OLTP-таблица `positions` с явным API.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService` (planned extension)

## Method (planned)

```proto
rpc GetPositions(GetPositionsRequest) returns (GetPositionsResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `GET /v1/positions` → gRPC)
- [risk-manager](../../05-components/risk-manager/overview.md)

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

TODO. Предлагаемая форма:

```proto
message Position {
  string symbol = 1;
  fob.common.v1.Decimal qty = 2;
  fob.common.v1.Decimal avg_entry_price = 3;
  fob.common.v1.Decimal unrealized_pnl = 4;
  fob.common.v1.Decimal realized_pnl = 5;
}

message GetPositionsRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
  repeated string symbols = 3;          // optional filter
}

message GetPositionsResponse {
  fob.common.v1.EventMeta meta = 1;
  repeated Position positions = 2;
}
```

## Used In Features

- [F-06. Positions / PnL / Margin](../../02-system/features/F-06-positions-pnl-margin/)

## Used In Use Cases

- [UC-F06-01](../../02-system/use-cases/UC-F06-01-show-positions/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F06-UC-F06-01-services](../../05-components/sequences/SEQ-F06-UC-F06-01-services.md)

## Related Components

- [ledger](../../05-components/ledger/overview.md)
- [gateway](../../05-components/gateway/overview.md)

## Related Data Objects

- [`positions`](../../07-data/oltp-schema.md#таблица-positions)

## Source Fragments

- IN-001-FR-016 (FR-LDG-005 positions, derived)
- IN-001-FR-022
