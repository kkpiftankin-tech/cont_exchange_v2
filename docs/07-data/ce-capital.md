---
id: DOC-DATA-CE-CAPITAL
phase: 07-data
status: schema-proposed-impl-pending
owner: core-team
source:
  - ADR-054 §10 «Определение позиции (NOP) и размещение по сервисам»
related:
  - docs/07-data/ce-position.md
  - docs/02-system/features/F-18-ce-capital-position-hedge/
  - contracts/proto/fob/ledger/v1/ledger.proto
---

# Капитал биржи CE = house-аккаунт в `ledger`

> **Размещение (ADR-054 §10):** капитал и валютные остатки биржи — это **балансы**,
> а балансами владеет `ledger`. Биржа моделируется **house-аккаунтом**
> (`user_id = '__ce_house__'`) в существующих балансовых таблицах ledger — БЕЗ
> новой «house-книги» в matching (снятая интерим-реализация `ce_capital`/
> `ce_position` в matching отменена §10).

## Owner

[ledger](../05-components/ledger/overview.md) — единственный writer балансов
(клиентских и house). Читатели: `risk` (для NOP), `frontend` (через ledger gRPC).

## Модель (переиспользование существующей машинерии ledger)

Никакой новой таблицы «капитала» не вводится. Капитал/остатки биржи ведутся в тех
же структурах, что и клиентские балансы:

| Что | Где в ledger | Смысл для house-аккаунта |
|---|---|---|
| Валютные остатки (собственные) | `accounts(user_id, asset, free_balance, reserved_balance)` / in-mem `balances_` | остатки биржи по каждой валюте; `user_id='__ce_house__'` |
| Остатки на внешних венью | `venue_balances_` (in-mem; **персистить** — см. ниже) | «собственные средства биржи на venue» (уже двигаются на `execution.venue`) |
| Стартовый капитал (seed) | seed в `accounts['__ce_house__']` при старте ledger | из env `CE_CAPITAL_SEED_<CCY>` |

**Схемных изменений почти нет:** `accounts` уже пускает `user_id='__ce_house__'`
(единственный CHECK — `user_id <> ''`, `infra/postgres/init.sql`), UNIQUE(user_id,asset).
`ledger_positions(user_id,currency,amount)` — тоже (без CHECK).

## Персистентность venue_balances (решение реализации)

`venue_balances_` сейчас **только in-memory** (нет таблицы, теряется при рестарте).
Для устойчивого NOP — один из вариантов (выбор на шаге кода):

1. **Kafka-replay** (ledger уже восстанавливает `balances_` реплеем `execution.venue`)
   — минимальные изменения, «истина = поток».
2. Таблица `venue_balances(venue, currency, total, reserved, updated_at)` —
   явная персистентность (переживает потерю ретенции топика).

## Seed капитала (env)

`CE_CAPITAL_SEED_<CCY>` (напр. `CE_CAPITAL_SEED_BTC=10`, `CE_CAPITAL_SEED_USDT=1000000`)
читается **ledger** при старте и заводит остатки house-аккаунта (idempotent —
только пустой аккаунт). Ранее эти env читал matching — переносится в ledger (§10).

## Связи

- Позиция (NOP) — производная от этих балансов, считается `risk`: см.
  [ce-position.md](ce-position.md).
- Контракт чтения: `fob.ledger.v1.LedgerService.GetExchangeBalances` (планируется).
