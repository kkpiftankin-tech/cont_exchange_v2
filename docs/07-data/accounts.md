---
id: DOC-DATA-ACCOUNTS
phase: 07-data
status: schema-ready-impl-pending
owner: core-team
source:
  - IN-001 «БД 1: PostgreSQL (OLTP)» §accounts
related:
  - docs/07-data/oltp-schema.md
  - docs/07-data/positions.md
  - docs/02-system/features/F-06-positions-pnl-margin/
  - docs/implementation-plan/F-06-positions-pnl-margin.tasks.md
---

# PostgreSQL Table: `accounts`

> **Status:** ✅ DDL добавлена в [`infra/postgres/init.sql`](../../infra/postgres/init.sql) (F-06, T-F06-010). C++ репозиторий (`PostgresAccountRepository`) — T-F06-020.
> Канонический per-field разбор — в [`oltp-schema.md` §accounts](oltp-schema.md#таблица-accounts). Здесь — owner/governance-карточка по skill `register-pg-table`.

| Свойство | Значение |
|---|---|
| База | PostgreSQL |
| Owner-сервис | ledger (Collateral & Ledger) |
| Writers | ledger |
| Readers | risk, gateway |
| Создаётся | `infra/postgres/init.sql:690` |

## Назначение

Счета клиентского коллатерала, по одной строке на пару `(user_id, asset)`. Хранит доступный/зарезервированный баланс, средства, размещённые на внешних venue, и in-flight переводы. Единственный writer — ledger; risk читает для расчёта margin, gateway — для отображения балансов.

## DDL (actual — `infra/postgres/init.sql:690`)

```sql
CREATE TABLE IF NOT EXISTS accounts (
  account_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id           TEXT NOT NULL,
  asset             TEXT NOT NULL,
  free_balance      NUMERIC(38, 18) NOT NULL DEFAULT 0,
  reserved_balance  NUMERIC(38, 18) NOT NULL DEFAULT 0,
  venue_allocated   NUMERIC(38, 18) NOT NULL DEFAULT 0,
  pending_transfer  NUMERIC(38, 18) NOT NULL DEFAULT 0,
  updated_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  CONSTRAINT accounts_free_balance_nonneg     CHECK (free_balance >= 0),
  CONSTRAINT accounts_reserved_balance_nonneg CHECK (reserved_balance >= 0),
  CONSTRAINT accounts_user_asset_unique       UNIQUE (user_id, asset)
);
CREATE INDEX IF NOT EXISTS idx_accounts_user_asset ON accounts (user_id, asset);
```

## Инварианты

- INV-1: `free_balance >= 0` — CHECK `accounts_free_balance_nonneg`.
- INV-2: `reserved_balance >= 0` — CHECK `accounts_reserved_balance_nonneg`.
- INV-3: ровно одна строка на `(user_id, asset)` — UNIQUE `accounts_user_asset_unique`.

## Когда заполняется

| Событие | Writer | Operation |
|---|---|---|
| Seed dev | init.sql | INSERT demo-user (10000 USDT, 1 BTC) |
| ApplyBatchResult / SettleFill | ledger | UPDATE WHERE (user_id, asset) |
| Reserve / Release funds | ledger | UPDATE free/reserved |

## Когда читается

| Query | Reader | Index used |
|---|---|---|
| балансы пользователя | ledger, gateway | `idx_accounts_user_asset` |
| collateral для margin | risk | `idx_accounts_user_asset` |

## Retention

Never deleted in MVP; обнуляется при закрытии счёта (вне scope F-06).

## Связанные feature.yaml

- [F-06](../02-system/features/F-06-positions-pnl-margin/feature.yaml)

## Известные ограничения

- `user_id` — TEXT без FK на `users` (таблицы `users` ещё нет в init.sql; совпадает со стилем `flow_orders.user_id`). FK добавится вместе с Auth/Identity.
- **Не путать** с runtime-таблицей `ledger_positions` (`user_id, currency, amount`), которую создаёт `cpp/ledger/src/infra/postgres_repositories.cpp` — это legacy per-currency баланс, не F-06 `accounts`.
