<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-LEDGER-002-apply-fill-position-update
level: fish
component: ledger
---
-->

# SEQ-LEDGER-002. Internal: ApplyBatchResult → positions/accounts update (flip + mark-to-market)

## Type

Internal Component Sequence (внутри `LedgerUseCases::ApplyBatchResult`, по одному `FillEvent`)

## Feature

- [F-06](../../../02-system/features/F-06-positions-pnl-margin/)

## Use Case

- [UC-F06-01](../../../02-system/use-cases/UC-F06-01-show-positions/use-case.md)

## Purpose

Детализирует обновление `positions` и `accounts` при потреблении одного
`FillEvent` из `batch.outputs`: idempotency-проверка, списание `reserved` /
начисление `free`, апдейт позиции (увеличение / уменьшение / **flip** через
ноль), расчёт `realized_pnl` на закрываемом объёме и пересчёт `avg_entry_price`.
Mark-to-market `unrealized_pnl` пересчитывается на текущей mark price в той же
транзакции (точка для последующего RiskSnapshot).

Service-level (cross-component, `matching → Kafka → ledger`) — см.
[SEQ-F04-UC-F04-01-services](../../sequences/SEQ-F04-UC-F04-01-services.md).
Требование: `positions`/`accounts` обновляются после каждого `BatchResult`
(F6-1).

## Diagram

```mermaid
sequenceDiagram
    participant CON as KafkaConsumer (batch.outputs)
    participant UC as LedgerUseCases::ApplyBatchResult
    participant IDEM as IdempotencyGuard
    participant AR as AccountRepository
    participant PR as PositionRepository
    participant PNL as PnlCalculator
    participant MD as MarkPriceProvider
    participant PG as PostgreSQL (TX)

    CON->>UC: ApplyBatchResult(batch_id, fills[])
    loop по каждому FillEvent
        UC->>IDEM: Seen(batch_id, order_id, fill_id)?
        alt уже применён
            IDEM-->>UC: true
            UC-->>UC: skip fill (idempotent)
        else новый fill
            IDEM-->>UC: false
            UC->>PG: BEGIN
            UC->>AR: SettleFill(user_id, fill)
            Note over AR: BUY:  reserved -= notional; free(base) += qty<br/>SELL: free(base) -= qty; free(quote) += notional<br/>release остатка reserved (см. known issue buy-reserve-leak)
            AR->>PG: UPDATE accounts ...
            UC->>PR: LoadForUpdate(user_id, symbol)
            PR->>PG: SELECT ... FOR UPDATE
            PG-->>PR: Position (current)

            alt same direction (наращивание)
                UC->>PNL: NewAvgEntry(qty0, entry0, fill_qty, fill_price)
                PNL-->>UC: avg_entry_price'
                Note over UC: quantity += fill_qty; realized_pnl без изменений
            else opposite, |fill_qty| <= |quantity| (частичное/полное закрытие)
                UC->>PNL: Realized(side, closed_qty, entry0, fill_price)
                PNL-->>UC: realized_delta
                Note over UC: quantity -= closed_qty; realized_pnl += realized_delta<br/>avg_entry_price без изменений; side='flat' если quantity==0
            else opposite, |fill_qty| > |quantity| (FLIP через ноль)
                UC->>PNL: Realized(side, quantity, entry0, fill_price)
                PNL-->>UC: realized_delta (на закрытом объёме)
                Note over UC: realized_pnl += realized_delta<br/>side := opposite; quantity := fill_qty - quantity0<br/>avg_entry_price := fill_price (новая нога)
            end

            UC->>MD: GetMarkPrice(symbol)
            MD-->>UC: mark_price
            UC->>PNL: ComputeUnrealized(side', quantity', avg_entry', mark_price)
            PNL-->>UC: unrealized_pnl'
            UC->>PR: Upsert(position')
            PR->>PG: INSERT ... ON CONFLICT (user_id, symbol) DO UPDATE
            UC->>IDEM: Mark(batch_id, order_id, fill_id)
            UC->>PG: COMMIT
        end
    end
    UC-->>CON: ack (offset commit после успешной обработки)
```

## Position transition rules (domain)

- **Increase** (`fill.side == position.side`): `quantity += fill_qty`,
  `avg_entry_price` пересчитывается как weighted average, `realized_pnl`
  не меняется.
- **Reduce** (`fill.side` противоположна, `fill_qty <= |quantity|`):
  `quantity -= closed_qty`, `realized_pnl += realized`, `avg_entry_price`
  сохраняется. При `quantity == 0` → `side = 'flat'`.
- **Flip** (`fill_qty > |quantity|`): закрываем всю текущую позицию (фиксируем
  `realized`), затем открываем остаток в противоположную сторону по
  `avg_entry_price = fill_price`.

Инвариант: `realized_pnl` начисляется только на фактически закрытом объёме;
двойное применение одного `fill_id` исключено `IdempotencyGuard` (CLAUDE.md §17).

## Idempotency & ordering

- Ключ идемпотентности — `(batch_id, order_id, fill_id)` (CLAUDE.md §13).
- Offset коммитится только после успешного `COMMIT` (at-least-once + idempotent
  consumer).
- Вся обработка одного fill — в одной PostgreSQL-транзакции; частичный апдейт
  `accounts` без `positions` невозможен.

## Decimal / precision notes (CLAUDE.md §9)

- `realized` / `unrealized` / `avg_entry_price` — `Decimal`, scale зафиксирован.
- Учитывается known issue `scale-inflation` (`Decimal::mul` удваивает scale):
  результат нормализуется к каноническому scale валюты перед записью.

## Related Contracts

- [`fob.ledger.v1.LedgerService/ApplyBatchResult`](../../../06-api/grpc/ledger-apply-batch-result.md)
- [batch.outputs](../../../06-api/messaging/batch-outputs.md)

## Related Data Objects

- [`positions`](../../../07-data/oltp-schema.md#таблица-positions)
- [`accounts`](../../../07-data/oltp-schema.md#таблица-accounts)

## Related Components

- [ledger](../overview.md)
- [matching-fob-core](../../matching-fob-core/overview.md) — producer `batch.outputs` (external)
- [market-data](../../market-data/overview.md) — mark price (external)
