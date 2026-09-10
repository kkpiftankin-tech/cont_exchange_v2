# gRPC Method: LedgerService/GetNodeBalances

## Status

Planned (контракт добавлен в `contracts/proto/fob/ledger/v1/ledger.proto`; реализация — T-CEA-201, ADR-057)

## Purpose

Вернуть **узловые остатки** «валюта@площадка» — средний уровень трёхуровневой позиции CE
(ADR-057): `(asset, venue, qty, target, in_transit)`. Нужен клирингу для проверки
исполнимости ноги (нельзя продать на OKX биткойн, лежащий на Kraken) и risk для расчёта
агрегата `Z_a = Σ_venue qty + in_transit + committed − target`.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc GetNodeBalances(GetNodeBalancesRequest) returns (GetNodeBalancesResponse);
```

## Caller

- matching (исполнимость ноги перед эмиссией)
- [risk-manager](../../05-components/risk-manager/overview.md) (`Z_a`)

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

```proto
message NodeBalance {
  string asset = 1;
  string venue = 2;                            // "Binance" | "__book__"
  fob.common.v1.Decimal qty = 3;
  fob.common.v1.Decimal target = 4;
  fob.common.v1.Decimal in_transit = 5;
}
message GetNodeBalancesRequest {
  fob.common.v1.EventMeta meta = 1;
  repeated string assets = 2;                  // пусто = все активы
}
message GetNodeBalancesResponse {
  fob.common.v1.EventMeta meta = 1;
  repeated NodeBalance nodes = 2;
}
```

## Used In Features

- [F-05A. Vectorized External Liquidity](../../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Used In Use Cases

- [UC-F05A-06 Run CE Tact with Virtual Counterparties](../../02-system/use-cases/UC-F05A-06-ce-agent-tact-clearing/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F05A-UC-F05A-06-services](../../05-components/sequences/SEQ-F05A-UC-F05A-06-services.md)

## Related Components

- [ledger](../../05-components/ledger/overview.md), matching, [risk-manager](../../05-components/risk-manager/overview.md)

## Related Data Objects

- PG `node_balances(account, asset, venue, qty, target, in_transit)` (planned, T-CEA-202)

## Related ADR

- [ADR-057 трёхуровневая позиция](../../03-architecture/adr/ADR-057-ce-three-level-position.md)
