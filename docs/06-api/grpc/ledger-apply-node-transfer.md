# gRPC Method: LedgerService/ApplyNodeTransfer

## Status

Implemented (2026-10-22, [ADR-084](../../03-architecture/adr/ADR-084-ce-link-physical-transfer.md)): смоделированный перевод в ledger. Контракт — `contracts/proto/fob/ledger/v1/ledger.proto`, первоначальное решение — T-CEA-201, ADR-057.

## Purpose

Применить **перевод валюты между узлами** (действие связки-арбитражёра, ADR-055). Заявка ставится в очередь
ledger и исполняется через `CE_TRANSFER_ETA_BATCHES` тактов клиринга (по умолчанию 3): остаток `from_venue`
уменьшается на `qty`, остаток `to_venue` увеличивается на `qty·(1 − fee)`, где `fee = CE_TRANSFER_SIM_FEE_BPS / 10⁴`
(по умолчанию 0). До исполнения приход виден в `NodeBalance.in_transit` назначения (справка: в `qty` и в сумму по
валюте он не входит). Идемпотентно по `transfer_id`. Перевод
**не меняет** актив-агрегат `Z_a` (меняет только географию), но меняет исполнимость ног на
конкретных площадках (ADR-057 §5, тождество (Ф4)).

## Поведение и ошибки

| Случай | Результат |
| --- | --- |
| корректная заявка | `OK`, поставлена в очередь |
| повтор с тем же `transfer_id` | `OK`, без эффекта (идемпотентность) |
| пустой `transfer_id`, `asset`, `from_venue` или `to_venue`; `from_venue = to_venue`; `qty ≤ 0`; масштаб `qty` вне 0…12 | `INVALID_ARGUMENT` |
| перезапуск ledger | очередь и остатки бирж (в памяти) обнуляются вместе |

Внутри ledger тот же путь использует решение связки: при `CE_ARB_TRANSFER_ENABLED=1` ledger сам ставит перевод с `transfer_id = xfer|агент|актив|назначение|batch_id` (порог, сумма и защиты — ADR-084, карточка [CALC-CE-LINK-TRANSFER](../../04-domain/calculations/CALC-CE-LINK-TRANSFER.md)). Журнал: `F-18 link transfer decision`, `F-18 node transfer queued`, `F-18 node transfer settled`.

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
