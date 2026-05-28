---
id: ADR-016
status: accepted
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - docs/02-system/features/F-20-live-venue-simulator/feature.yaml
  - docs/03-architecture/adr/ADR-015-sim-execution-topic-isolation.md
  - cpp/ledger/src/app/ledger_uc.cpp
  - cpp/ledger/src/infra/postgres_repositories.cpp
  - docs/07-data/oltp-schema.md
---

# ADR-016: Ledger sim-book — отдельные таблицы vs simMode-флаг в существующих

## Контекст

F-20 требует, чтобы `SimExecutionReport` (simMode=true) обновлял
**изолированную** sim-книгу позиций/балансов, НЕ затрагивая боевые
позиции провайдера (acceptance F20-5, DoD-7).

Ledger сейчас хранит:

- `positions` (user → instrument → amount/avg_entry/realised_pnl) —
  в памяти + PG (через PositionsRepository);
- `balances` (user → currency → available/reserved);
- PG `hedgeflows.hedge_pnl` / `tot_fee` (через PostgresHedgeflowPnlSink,
  PR-F12-3c).

Вопрос: как хранить sim-эквивалент?

- **Вариант A:** добавить `sim_mode BOOLEAN` колонку в существующие
  таблицы (positions, balances, hedgeflows), фильтровать везде.
- **Вариант B:** отдельные таблицы `sim_positions`, `sim_balances`,
  `sim_hedge_pnl` (или sim-namespace), которые трогает только sim-book
  consumer.

## Решение

**Вариант B — отдельные sim-таблицы.** SimExecutionReport (читается из
`sim.execution.venue`, ADR-015) обновляет:

- `sim_positions` (provider → instrument → net/avg/realised, +
  `sim_session_id`);
- `sim_balances` (provider → currency, опционально — для MVP может
  быть отложено);
- `sim_hedge_pnl` (агрегат hedge_pnl/fee по sim_session_id) — sim-аналог
  PostgresHedgeflowPnlSink.

Боевые `positions` / `balances` / `hedgeflows` НЕ получают `sim_mode`
колонку и НЕ трогаются sim-путём.

## Почему НЕ simMode-флаг в существующих таблицах

1. **Финансовая безопасность (тот же принцип, что ADR-015).** Флаг
   означает, что КАЖДЫЙ боевой запрос — GetBalances, GetPositions,
   GetUnrealisedPnL, risk margin/liquidation checks — должен добавить
   `WHERE sim_mode = false`. Один забытый фильтр → margin call или
   liquidation на основе sim-позиции. Отдельные таблицы делают это
   **физически невозможным**: боевой запрос обращается к `positions`,
   sim-запрос к `sim_positions`, пересечения нет.
2. **Разные модели жизненного цикла.** Sim-позиции привязаны к
   `sim_session_id` и должны обнуляться/архивироваться при завершении
   SimSession (status=COMPLETED). Боевые позиции живут вечно. Смешение
   в одной таблице усложняет retention и cleanup.
3. **CLAUDE.md §17 (Ledger rules).** Явно: «смешивать user balance и
   exchange hedge balance — запрещено», «settlement на основе
   floating-point money — запрещено». Sim-book — это ещё одна
   изолированная книга; отдельные таблицы соответствуют духу правила.

## Цена решения

- Дублирование схемы (DDL для sim_positions ≈ positions). Одноразовая
  стоимость.
- sim-book ledger логика — отдельная ветка кода
  (`LedgerUseCases::ApplySimExecutionReport` или отдельный
  `SimLedgerUseCases`), читающая `sim.execution.venue`.

Это приемлемо против постоянного риска забытого фильтра.

## Реализация (phased)

| Phase | Решение |
| --- | --- |
| F-20 MVP | `sim_positions` + `sim_hedge_pnl` в init.sql; sim-book consumer на `sim.execution.venue`; `sim_balances` отложить (sim не резервирует реальные средства) |
| F-20 v1.1 | `sim_balances` если потребуется sim-margin / sim-collateral моделирование |

Идемпотентность sim-book — по `report_id` (как боевой ledger), но в
отдельном `sim_seen_report_keys` множестве/таблице.

## Последствия

### Положительные

- Невозможно случайно применить sim к боевым позициям.
- Чистый sim-teardown по sim_session_id.
- Боевой ledger код не меняется (только добавляется sim-ветка).

### Отрицательные

- Схема и часть логики дублируются.
- Если в будущем понадобится «boevое + sim в одном отчёте для UI» —
  нужен join по provider+symbol через два набора таблиц.

### Обратимость

Средняя. Схлопнуть в флаг-колонку позже можно (миграция), но это шаг
в сторону менее безопасного дизайна — вряд ли понадобится.

## Status

Accepted (2026-05-28). Резолвит F-20 knownIssue `ledger-sim-book-isolation`. Таблицы `sim_positions` / `sim_hedge_pnl` добавлены в `infra/postgres/init.sql` (F-20 Phase 2).
