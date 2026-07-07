---
id: DOC-DATA-POSITIONS
phase: 07-data
status: schema-ready-impl-pending
owner: core-team
source:
  - IN-001 «БД 1: PostgreSQL (OLTP)» §positions
related:
  - docs/07-data/oltp-schema.md
  - docs/07-data/accounts.md
  - docs/02-system/features/F-06-positions-pnl-margin/
  - docs/implementation-plan/F-06-positions-pnl-margin.tasks.md
---

# PostgreSQL Table: `positions`

> **Status:** ✅ DDL добавлена в [`infra/postgres/init.sql`](../../infra/postgres/init.sql) (F-06, T-F06-010). C++ репозиторий (`PostgresPositionRepository`) — T-F06-020.
> Канонический per-field разбор — в [`oltp-schema.md` §positions](oltp-schema.md#таблица-positions).

| Свойство | Значение |
|---|---|
| База | PostgreSQL |
| Owner-сервис | ledger |
| Writers | ledger |
| Readers | risk, gateway |
| Создаётся | `infra/postgres/init.sql:713` |

## Назначение

Текущая нетто-позиция по инструменту, по одной строке на `(user_id, symbol)`. Обновляется ledger при каждом `FillEvent` (increase / reduce / flip), хранит `avg_entry_price`, `realized_pnl` и mark-to-market `unrealized_pnl`. risk читает для margin/liquidation, gateway — для таблицы позиций.

## DDL (actual — `infra/postgres/init.sql:713`)

```sql
CREATE TABLE IF NOT EXISTS positions (
  position_id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id           TEXT NOT NULL,
  symbol            TEXT NOT NULL,
  side              TEXT NOT NULL DEFAULT 'flat'
                    CHECK (side IN ('long', 'short', 'flat')),
  quantity          NUMERIC(38, 18) NOT NULL DEFAULT 0,
  avg_entry_price   NUMERIC(38, 18) NOT NULL DEFAULT 0,
  unrealized_pnl    NUMERIC(38, 18) NOT NULL DEFAULT 0,
  realized_pnl      NUMERIC(38, 18) NOT NULL DEFAULT 0,
  updated_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  CONSTRAINT positions_quantity_nonneg    CHECK (quantity >= 0),
  CONSTRAINT positions_user_symbol_unique UNIQUE (user_id, symbol)
);
CREATE INDEX IF NOT EXISTS idx_positions_user_symbol ON positions (user_id, symbol);
```

## Инварианты

- INV-1: `quantity >= 0` (направление в `side`) — CHECK `positions_quantity_nonneg`.
- INV-2: `side IN ('long','short','flat')` — CHECK.
- INV-3: ровно одна строка на `(user_id, symbol)` — UNIQUE `positions_user_symbol_unique`.
- INV-4 (app-level, T-F06-022): flip через ноль фиксирует `realized_pnl` только на закрытом объёме; новая нога открывается по `fill_price`.

## Когда заполняется

| Событие | Writer | Operation |
|---|---|---|
| ApplyBatchResult (FillEvent) | ledger | UPSERT ON CONFLICT (user_id, symbol) |
| Mark-to-market | ledger | UPDATE unrealized_pnl |

## Когда читается

| Query | Reader | Index used |
|---|---|---|
| GetPositions(user_id) | ledger | `idx_positions_user_symbol` |
| margin / liquidation | risk | `idx_positions_user_symbol` |

## Retention

Never deleted в MVP; `flat`-позиции остаются строкой с `quantity=0`.

## Связанные feature.yaml

- [F-06](../02-system/features/F-06-positions-pnl-margin/feature.yaml)

## Известные ограничения

- `user_id` — TEXT без FK на `users` (нет таблицы `users`), совпадает со стилем `flow_orders`.
- **Не путать** с `sim_positions` (ADR-016, изолированный sim-book) и с runtime `ledger_positions` (legacy per-currency баланс).
