# gRPC Method: LedgerService/GetPositions

## Status

Implemented (контракт добавлен в `contracts/proto/fob/ledger/v1/ledger.proto`, F-06)

## Purpose

Получить текущие позиции пользователя по всем инструментам: `(symbol, qty, avg_entry_price, unrealized_pnl, realized_pnl)`. Используется UI/Gateway и Risk Manager.

В текущем MVP позиции выводятся косвенно из applied fills; целевая модель — отдельная OLTP-таблица `positions` с явным API.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc GetPositions(GetPositionsRequest) returns (GetPositionsResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `GET /v1/positions` → gRPC)
- [risk-manager](../../05-components/risk-manager/overview.md)

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

Финальный контракт (см. `contracts/proto/fob/ledger/v1/ledger.proto`):

```proto
// Monetary values use fob.common.v1.Decimal (same style as Balance / UnrealisedPnL).
message Position {
  string symbol = 1;                            // e.g., "BTC/USDT"
  string side = 2;                              // "long" | "short" | "flat"
  fob.common.v1.Decimal quantity = 3;           // current position size (absolute)
  fob.common.v1.Decimal avg_entry_price = 4;    // average entry price
  fob.common.v1.Decimal mark_price = 5;         // reference/mark price for valuation
  fob.common.v1.Decimal unrealized_pnl = 6;     // (mark_price - avg_entry_price) * signed qty
  fob.common.v1.Decimal realized_pnl = 7;       // cumulative realised PnL for this position
}

message GetPositionsRequest {
  string user_id = 1;
}

message GetPositionsResponse {
  repeated Position positions = 1;
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
