---
id: ADR-028
status: accepted
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - docs/03-architecture/adr/ADR-009-shadow-mode-isolation-strategy.md
  - docs/03-architecture/adr/ADR-022-batch-clearing-solver-replay.md
  - docs/02-system/features/F-15-backtest-replay/
  - cpp/backtest/
---

# ADR-028: Backtest/replay parity и audit mode

## Контекст

[ADR-009](ADR-009-shadow-mode-isolation-strategy.md) фиксирует изоляцию
shadow-ledger для F-15, но более широкая стратегия **parity** (replay даёт тот
же результат, что прод, при той же бизнес-логике) как отдельное архитектурное
решение не закреплена. F-15 backtest уже реализован (`cpp/backtest`), поэтому
фиксируем как `accepted`.

## Решение

- **Та же доменная логика**: replay использует тот же Matching / Risk / Ledger
  domain-слой, что и прод (reuse через CMake source dependency, не форк).
- **Shadow state isolation**: балансы/позиции replay изолированы
  ([ADR-009](ADR-009-shadow-mode-isolation-strategy.md)); replay не пишет в
  боевые топики (`execution.venue`, `fills`, ledger mutations, `risk.alerts`).
- **Deterministic batch ordering**: тот же вход батча → тот же `BatchResult`
  ([ADR-022](ADR-022-batch-clearing-solver-replay.md)).
- **Audit diff**: режим сравнения prod vs replay; допустимое расхождение
  (allowed divergence) задаётся явно (например, tolerance по residual_norm).
- **AgentLog requirements**: replay пишет `replay_agentlogs`
  ([ADR-010](ADR-010-agentlog-replay-agentlog-unification.md),
  [ADR-011](ADR-011-replay-agentlogs-ddl-placement.md)) для аудита и parity-проверок.

## Альтернативы

- **Отдельная реализация логики для replay** — отклонено: гарантированный drift между prod и replay.
- **Replay в боевые топики с флагом** — отклонено: риск загрязнения боевого состояния (тот же принцип, что ADR-015/016).

## Последствия

- **Плюс:** воспроизводимый backtest/audit; доверие к replay как к инструменту валидации (в т.ч. для смены solver/routing).
- **Минус:** требование детерминизма и изоляции ограничивает оптимизации и требует дисциплины reuse доменного слоя.

## Обратимость

Низкая по принципу (parity — фундамент backtest); реализация изоляции (in-memory namespace) обратима на PG-schema, см. [ADR-009](ADR-009-shadow-mode-isolation-strategy.md).
