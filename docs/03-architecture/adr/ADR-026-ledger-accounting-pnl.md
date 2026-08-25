---
id: ADR-026
status: accepted
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - contracts/proto/fob/ledger/v1/ledger.proto
  - docs/05-components/ledger/overview.md
  - docs/03-architecture/adr/ADR-005-fixed-point-decimal-money.md
  - docs/03-architecture/adr/ADR-020-event-ordering-idempotency.md
  - CLAUDE.md (§17 ledger rules)
---

# ADR-026: Ledger accounting и PnL source of truth

## Контекст

Ledger — критический компонент: source of truth по balances/positions
(CLAUDE.md §17). ADR-009/011 касаются replay/backtest-изоляции, но общего
ADR по самому ledger-учёту нет. Правила (no double-apply, idempotency,
no float money) разбросаны по §17 и коду.

## Решение

Зафиксировать модель ledger-учёта.

- **LedgerEntry immutable**: записи учёта не мутируются; корректировки — новыми записями.
- **Positions / balances** обновляются по правилам из fills и execution
  reports; user balance и exchange hedge balance **не** смешиваются.
- **Fees** и **hedge PnL** учитываются явно.
- **Деньги — fixed-point Decimal** ([ADR-005](ADR-005-fixed-point-decimal-money.md)); settlement на float запрещён.
- **Идемпотентность обязательна** ([ADR-020](ADR-020-event-ordering-idempotency.md)):
  fill не применяется дважды; резерв не освобождается без idempotency key
  (`report_id` / `reservation_id`). Exactly-once не предполагается.
- **Audit trail**: для prod ledger-пути фиксируются reservation_id, user_id,
  order_id, batch_id / execution_report_id, source topic/service, before/after
  snapshot.
- **Reconciliation** с внешним исполнением (execution reports) — обязательна.
- **Replay / shadow exceptions**: shadow/replay ledger изолирован
  ([ADR-009](ADR-009-shadow-mode-isolation-strategy.md)); sim-book изолирован
  ([ADR-016](ADR-016-ledger-sim-book-storage.md)). Боевой Account/Position/
  LedgerEntry не ссылается на replaySessionId/sim, кроме audit-метаданных.

## Альтернативы

- **Mutable balances без entry-журнала** — отклонено: теряется audit trail и reconciliation.
- **Exactly-once вместо идемпотентности** — отклонено (см. [ADR-020](ADR-020-event-ordering-idempotency.md)).
- **Float money** — отклонено ([ADR-005](ADR-005-fixed-point-decimal-money.md)).

## Последствия

- **Плюс:** аудируемый, реплеябельный, точный ledger; безопасен к дубликатам.
- **Минус:** стоимость хранения seen-keys/snapshots и дисциплина идемпотентности.

## Обратимость

Низкая. Ledger — source of truth денег; изменение модели учёта затрагивает risk, matching settlement и reconciliation.
