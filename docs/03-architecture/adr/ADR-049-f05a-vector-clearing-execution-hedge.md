---
id: ADR-049
title: F-05A vector clearing execution → F-12 hedge channel (converged-only, feature-flagged)
status: accepted
date: 2026-08-27
accepted: 2026-08-27
level: sea
feature: F-05A
related: [ADR-047, ADR-048, ADR-034, ADR-033]
---

# ADR-049 — F-05A vector clearing execution через F-12 hedge channel

> **Принято 2026-08-27** (решение владельца): сошедшийся векторный клиринг F-05A
> исполняется как **биржевой хедж** через F-12 (`ExecutionIntent` → venues →
> `ExecutionReport` → ledger `venue_balances_`/hedge-PnL), **за feature-flag
> `F05A_MONEY_ENABLED` (default off), только для converged** (residual ≤ tolerance).

## Контекст

F-05A векторизует **внешнюю venue-ликвидность** (`venue.liquidity.fob` →
`ExternalOrderLevel` → `VectorFlowSegment`). Сегменты несут `venue_id` +
`source_order_id`, но **не имеют `user_id`**. Все существующие денежные пути
(`ExecutionGroup`/`FlowFill` → `ledger.ApplyExecutionGroup`) жёстко пишут на
**балансы/позиции пользователя по `user_id`**. Кому идут fills при сошедшемся
клиринге, исходные доки (IN-014) **не определяют** — это money-model решение (§3.3).

House/`EXCHANGE_PNL`-счёта в ledger **нет** (подтверждено). Инвариант ADR-047 (§17):
ledger никогда не применяет несбалансированную группу.

## Решение

1. **Семантика.** Сошедшийся external-клиринг = **биржевой хедж/арбитраж** против
   площадок. Для каждого сегмента с `x_i > 0` эмитится **`ExecutionIntent`** в
   `execution.intents` (F-12): `venue = segment.venue_id`, `instrument` из
   `segment.pair`, `side` из сегмента, `target_qty = x_i`,
   **`limit_price = segment.effective_price`** (не reference-price — исполняется
   исходный external level, R-F05A-006), `reason = "f05a_vector_clearing"`,
   `batch_id` из клиринга. Далее venues → `ExecutionReport` → ledger применяет в
   **`venue_balances_` + hedge-PnL** (не user-балансы).

2. **Никаких user-проводок.** F-05A money-path **не** эмитит `ExecutionGroup`/
   `FlowFill` (нет `user_id`/counterparty). Подключение к user F-09 combo-ногам —
   отдельное решение (см. Альтернативы).

3. **Feature-flag.** Вся эмиссия за `Env::get_bool("F05A_MONEY_ENABLED", false)`
   (по образцу `grouped_enabled_`, F-09 MVP-7 default-off). Выкл по умолчанию →
   обратимо, деньги не двигаются до явного включения.

4. **Converged-only.** Эмитим **только** при
   `SurplusDecision.action == kProceedNoSurplus` (residual ≤ tolerance,
   сбалансировано). Любой значимый остаток → `kRejectIfResidual` (ADR-047 default)
   → **эмиссии нет** (conservation держится без house-счёта).

5. **Идемпотентность / трассировка.** `intent_id` детерминирован по
   `batch_id + segment_id` → повторная обработка не плодит дублей (F-12 execution
   FSM + idempotency по intent/report). Source-trace: `venue_id/source_order_id/
   segment_id/batch_id` (в `reason`/полях intent; полноценные `LegResult`
   source-trace поля — T-F05A-106, при переходе на user-проводки).

6. **Деньги (§9).** `target_qty`/`limit_price` — `Decimal`. `double` только в
   солвере/диагностике, в ledger не попадает.

## Альтернативы

- **(b) User-fills через F-09 `ExecutionGroup`.** Требует привязки клиринга к
  user/parent-ордеру (external-ликвидность как counter-liquidity user combo-ног).
  В коде сейчас F-05A — отдельный диагностический цикл, не подключён к
  `SolveGroupedBatchUseCase`. Отложено; при реализации user-ноги → `ExecutionGroup`,
  external-нога → тот же F-12 hedge-канал.
- **(c) House/синтетический счёт.** Заблокировано: house-счёта в ledger нет
  (нужен под-проект, T-F05A-401 / `EXCHANGE_PNL` ADR-047).

## Последствия

- Обратимо: flag default-off, без изменения схем. Включение — операторское решение.
- Переиспользуется готовая F-12-машинерия (`execution.intents`, venues,
  `ApplyExecutionReport`, hedge-PnL) — новых топиков/таблиц не нужно.
- Ограничение: покрывает только external-хедж-сторону; user-проводки и
  `EXCHANGE_PNL`-surplus — вне scope (отдельные решения).
- Требуется persistent-идемпотентность на execution-report пути (уже есть composite
  key + FSM в ledger) — конкретно для F-05A новых гарантий не добавляем.

## Обратимость

Высокая: отключается флагом (default-off); эмиссия — аддитивные `ExecutionIntent`
в существующий F-12-канал; откат = выключить флаг. Схемы БД не меняются.

## Трассировка

- Feature: [F-05A](../../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)
- Surplus/QP: [ADR-047](ADR-047-surplus-exchange-pnl-policy.md), [ADR-048](ADR-048-qp-solver-backend.md)
- Контракты: `contracts/proto/fob/execution/v1/execution.proto` (`ExecutionIntent`), топик `execution.intents`
- Код (при реализации): `cpp/matching/src/app/matching_loop.cpp` (`on_vectorized_liquidity`, flag-гейт), `cpp/matching/src/app/vector_clearing_hedge_builder.*` (segments+x → `ExecutionIntent[]`), `cpp/matching/src/infra/kafka/execution_intents_producer.*`
- Ledger: `cpp/ledger/src/app/ledger_uc.cpp` (`ApplyExecutionReport` → `venue_balances_`/hedge-PnL)
- Plan: `docs/implementation-plan/F-05A-vectorized-external-liquidity.tasks.md` (T-F05A-305 money-path)
