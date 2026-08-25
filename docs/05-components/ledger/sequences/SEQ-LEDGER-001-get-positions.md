<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-LEDGER-001-get-positions
level: fish
component: ledger
---
-->

# SEQ-LEDGER-001. Internal: GetPositions (build positions + unrealized PnL)

## Type

Internal Component Sequence (внутри `LedgerUseCases::GetPositions`)

## Feature

- [F-06](../../../02-system/features/F-06-positions-pnl-margin/)

## Use Case

- [UC-F06-01](../../../02-system/use-cases/UC-F06-01-show-positions/use-case.md)

## Purpose

Детализирует, что происходит внутри одного вызова `GetPositions(user_id)`:
как читаются строки `positions` и `accounts` из PostgreSQL, как для каждой
открытой позиции пересчитывается `unrealized_pnl` относительно текущей mark
price, и как собирается gRPC-ответ.

Service-level (cross-component) — см.
[SEQ-F06-UC-F06-01-services](../../sequences/SEQ-F06-UC-F06-01-services.md).

## Diagram

```mermaid
sequenceDiagram
    participant SVC as GrpcLedgerService
    participant UC as LedgerUseCases::GetPositions
    participant PR as PositionRepository
    participant AR as AccountRepository
    participant MD as MarkPriceProvider
    participant PNL as PnlCalculator
    participant PG as PostgreSQL

    SVC->>UC: GetPositions(user_id)
    UC->>PR: ListByUser(user_id)
    PR->>PG: SELECT * FROM positions WHERE user_id = $1
    PG-->>PR: rows[positions]
    PR-->>UC: vector<Position>
    UC->>AR: ListByUser(user_id)
    AR->>PG: SELECT asset, free_balance, reserved_balance,<br/>venue_allocated, pending_transfer FROM accounts WHERE user_id = $1
    PG-->>AR: rows[accounts]
    AR-->>UC: vector<Account>

    loop по каждой Position со side != 'flat'
        UC->>MD: GetMarkPrice(symbol)
        MD-->>UC: mark_price (Decimal) или stale-flag
        UC->>PNL: ComputeUnrealized(side, quantity, avg_entry_price, mark_price)
        Note over PNL: long:  (mark - entry) * qty<br/>short: (entry - mark) * qty<br/>Decimal fixed-point, scale зафиксирован
        PNL-->>UC: unrealized_pnl
    end

    UC-->>SVC: GetPositionsResponse{positions[], accounts[], as_of}
```

## Internal failure handling

- **Нет строк в `positions`** → возврат пустого списка позиций (не ошибка);
  балансы из `accounts` всё равно возвращаются.
- **Mark price отсутствует / устарел** (`MarkPriceProvider` вернул stale-flag)
  → `unrealized_pnl` берётся из последнего сохранённого `positions.unrealized_pnl`,
  ответ помечается `mark_stale=true`, пишется WARN-лог. PnL не обнуляется молча.
- **`side = 'flat'` или `quantity = 0`** → `unrealized_pnl = 0`, mark price не
  запрашивается.
- **Ошибка чтения PostgreSQL** → typed `Error{code, message}`, частичный ответ
  не отдаётся.

## Decimal / precision notes (CLAUDE.md §9)

- Все денежные величины — `Decimal` (fixed-point), `double` запрещён.
- `ComputeUnrealized` явно фиксирует scale результата и не смешивает base/quote
  amount с price; покрывается unit-тестами U1–U3 (см.
  [F-06 tasks](../../../implementation-plan/F-06-positions-pnl-margin.tasks.md)).

## Related Contracts

- [`fob.ledger.v1.LedgerService/GetPositions`](../../../06-api/grpc/ledger-get-positions.md)
- [`fob.ledger.v1.LedgerService/GetBalances`](../../../06-api/grpc/ledger-get-balances.md)

## Related Data Objects

- [`positions`](../../../07-data/oltp-schema.md#таблица-positions)
- [`accounts`](../../../07-data/oltp-schema.md#таблица-accounts)

## Related Components

- [ledger](../overview.md)
- [market-data](../../market-data/overview.md) — источник mark price (external)
