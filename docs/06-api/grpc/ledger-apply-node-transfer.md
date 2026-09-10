# gRPC Method: LedgerService/ApplyNodeTransfer

## Status

Planned (контракт добавлен в `contracts/proto/fob/ledger/v1/ledger.proto`; реализация — T-CEA-201, ADR-057)

## Purpose

Применить **перевод валюты между узлами** (действие связки-арбитражёра, ADR-055): списать с
`from_venue`, зачислить в `in_transit` `to_venue`. Идемпотентно по `transfer_id`. Перевод
**не меняет** актив-агрегат `Z_a` (меняет только географию), но меняет исполнимость ног на
конкретных площадках (ADR-057 §5, тождество (Ф4)).

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc ApplyNodeTransfer(ApplyNodeTransferRequest) returns (google.protobuf.Empty);
```

## Caller

- venues / [ledger](../../05-components/ledger/overview.md) consumer (по подтверждению перевода)

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

```proto
message ApplyNodeTransferRequest {
  fob.common.v1.EventMeta meta = 1;
  string transfer_id = 2;                      // idempotency key
  string asset = 3;
  string from_venue = 4;
  string to_venue = 5;
  fob.common.v1.Decimal qty = 6;
}
```

## Used In Features

- [F-05A. Vectorized External Liquidity](../../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Used In Use Cases

- [UC-F05A-06 Run CE Tact with Virtual Counterparties](../../02-system/use-cases/UC-F05A-06-ce-agent-tact-clearing/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F05A-UC-F05A-06-services](../../05-components/sequences/SEQ-F05A-UC-F05A-06-services.md)

## Related Components

- [ledger](../../05-components/ledger/overview.md), venues

## Related Data Objects

- PG `node_balances` (in_transit); CH `node_balances_history` (planned, T-CEA-202/401)

## Related ADR

- [ADR-055 виртуальные контрагенты](../../03-architecture/adr/ADR-055-ce-virtual-counterparties-agents.md), [ADR-057 трёхуровневая позиция](../../03-architecture/adr/ADR-057-ce-three-level-position.md)
