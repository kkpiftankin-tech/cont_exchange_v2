---
id: ADR-044
title: Модель балансов ledger — единый PG-источник vs dual in-memory + PG
status: proposed
date: 2026-06-29
level: sea
feature: F-06
related: [ADR-026, ADR-020, ADR-005, ADR-030]
---

# ADR-044 — Модель балансов ledger: единый PG-источник истины vs dual in-memory + PG

## Контекст

В ledger сейчас сосуществуют **два независимых учётных контура** для одних и тех же
денежных величин:

1. **Legacy in-memory** (`balances_` / `reservations_` в ledger): order_flow при
   размещении ордера вызывает gRPC `ReserveFunds` → ledger двигает
   `available → reserved` **только в памяти**. `ReleaseFunds` так же работает по
   in-memory структурам. Это исторический MVP-контур (no-persistence known issue,
   [ADR-030](ADR-030-legacy-mvp-migration-policy.md)).
2. **Новая PG-таблица `accounts`** (F-06, `infra/postgres/init.sql`): пишется
   **только** на филле — `ApplyFillTx` / `ApplyBatchResult` при потреблении
   `batch.outputs` списывает `reserved_balance` и начисляет `free_balance` атомарной
   транзакцией.

**Дефект.** Резерв при размещении ордера **не зеркалится** в `accounts`: `accounts`
узнаёт о средствах только в момент филла. При филле `ApplyFillTx` пытается списать
`reserved_balance`, которого в `accounts` никогда не было → значение уходит в минус →
нарушение `CHECK accounts_reserved_balance_nonneg`. Сейчас это **временно
замаскировано** `GREATEST(0, reserved_balance - executed_notional)` в SQL-проводке,
что прячет рассинхрон, но не устраняет его: два контура расходятся, а `accounts`
не является честным источником истины по `reserved`.

Это блокирует F-06 (positions/PnL/margin): risk при построении `RiskSnapshot` читает
`free_collateral` / `reserved_collateral` из `accounts`, а они недостоверны, пока
резервы живут только в памяти.

## Решение

Принять **переходную модель «mirror»** с явной целью схождения на единый PG-источник:

1. **Переходное состояние (mirror).** `ReserveFunds` и `ReleaseFunds` дополнительно
   зеркалятся в `accounts` **атомарными PG-проводками** (`free → reserved` при
   reserve, `reserved → free` при release), идемпотентными по `reservation_id`
   ([ADR-020](ADR-020-event-ordering-idempotency.md)). Тем самым `accounts.reserved_balance`
   отражает реальные открытые резервы **до** филла, и `ApplyFillTx` списывает
   существующий резерв без ухода в минус. Маскирующий `GREATEST(0, …)` убирается
   после стабилизации mirror (резерв перестаёт быть отрицательным «по построению»).
2. **In-memory становится производным.** `balances_` / `reservations_` сохраняются
   как кэш для горячего пути и обратной совместимости gRPC, но **источник истины —
   `accounts`**. На рестарте кэш гидрируется из PG (закрывает known issue
   `no-persistence`).
3. **Целевое состояние.** `accounts` — единственный источник истины по
   `free`/`reserved`/`total`; in-memory-контур удаляется или вырождается в read-кэш.
   Все денежные величины — fixed-point `Decimal`
   ([ADR-005](ADR-005-fixed-point-decimal-money.md)); смешение user-баланса и
   exchange-hedge-баланса запрещено (CLAUDE.md §17, [ADR-026](ADR-026-ledger-accounting-pnl.md)).

Инвариант после перехода: для каждого `(user_id, asset)` в любой момент
`reserved_balance = Σ открытых резервов`, и `reserved` не может стать отрицательным
без снятия (release) или применения (apply) конкретного `reservation_id`.

## Альтернативы

- **Оставить dual (in-memory резерв + PG только на филле).** Отклонено: рассинхрон
  фундаментален, `accounts.reserved` остаётся недостоверным, маскировка `GREATEST(0,…)`
  скрывает баги вместо их устранения; risk-margin строится на ложных данных.
- **Сразу полный перенос на PG без mirror-фазы** (удалить in-memory, все
  reserve/release — только PG-проводки одним PR). Отклонено для текущего этапа:
  затрагивает горячий путь `ReserveFunds` на каждом размещении ордера и порядок
  взаимодействия order_flow ↔ ledger; высокий риск регрессий при одномоментной
  замене. Mirror даёт безопасный инкрементальный путь к этой же цели.

## Последствия

- ✅ `accounts.reserved_balance` становится достоверным до филла → корректные
  `free_collateral` / `reserved_collateral` для F-06 risk-снапшота.
- ✅ Снимается маскировка `GREATEST(0,…)`; нарушение `CHECK …_reserved_balance_nonneg`
  больше не «лечится» обрезкой.
- ✅ Балансы переживают рестарт (гидрация кэша из PG) — закрывает `no-persistence`.
- ⚠️ `ReserveFunds`/`ReleaseFunds` получают PG-запись в горячем пути → латентность
  и нагрузка на соединения (см. [ADR-045](ADR-045-pg-connection-pooling.md): пул
  соединений становится предусловием).
- ⚠️ Переходный период: два контура существуют одновременно; нужен reconciliation-чек
  (in-memory vs `accounts`) до удаления кэша.
- ⚠️ Идемпотентность reserve/release/apply по `reservation_id` / `(batch_id,order_id,fill_id)`
  обязательна, иначе at-least-once-доставка `batch.outputs` задвоит проводки.

## Обратимость

Средняя на mirror-фазе (mirror-проводки можно отключить флагом, вернувшись к
dual-поведению, пока кэш не удалён), **низкая** после удаления in-memory-контура:
ledger — источник истины денег ([ADR-026](ADR-026-ledger-accounting-pnl.md)),
откат к in-memory потребует повторной миграции состояния из PG.

## Трассировка

- Feature: [F-06](../../02-system/features/F-06-positions-pnl-margin/feature.yaml)
- Data: [`accounts`](../../07-data/oltp-schema.md#таблица-accounts) (`infra/postgres/init.sql`)
- Contracts: `fob.ledger.v1.LedgerService.ReserveFunds` / `ReleaseFunds` /
  `ApplyBatchResult` ([contracts/proto/fob/ledger/v1/ledger.proto](../../../contracts/proto/fob/ledger/v1/ledger.proto))
- Messaging: [batch.outputs](../../06-api/messaging/batch-outputs.md)
- Related: [ADR-026](ADR-026-ledger-accounting-pnl.md) (ledger source of truth),
  [ADR-020](ADR-020-event-ordering-idempotency.md) (idempotency),
  [ADR-005](ADR-005-fixed-point-decimal-money.md) (Decimal),
  [ADR-030](ADR-030-legacy-mvp-migration-policy.md) (legacy migration),
  [ADR-045](ADR-045-pg-connection-pooling.md) (PG-пул как предусловие mirror).
