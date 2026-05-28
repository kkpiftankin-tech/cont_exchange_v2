---
id: ADR-022
status: accepted
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - contracts/proto/fob/matching/v1/batch.proto
  - docs/03-architecture/adr/ADR-028-backtest-replay-parity.md
  - docs/05-components/matching-fob-core/overview.md
  - CLAUDE.md (§8.4 BatchResult, §15 matching rules)
---

# ADR-022: Batch clearing solver и deterministic replay

## Контекст

`matching` сейчас — batch-clearing **симулятор** (CLAUDE.md §15, §26), но он
уже эмитит `BatchResult` (`contracts/proto/fob/matching/v1/batch.proto`) с
диагностикой (`residual_norm`, `solve_time_ms`, `num_active_orders`,
`config_version`), и F-15 требует детерминированного replay. Контракт и
требования к детерминизму нужно зафиксировать **до** замены симулятора
реальным solver, чтобы переход не сломал replay/ledger.

## Решение

Зафиксировать контракт и инварианты clearing-цикла независимо от конкретного
алгоритма solver:

- **Batch interval**: clearing выполняется периодически; вход батча
  «замораживается» (solver input freeze) на момент цикла.
- **Deterministic ordering**: при одинаковом входе батча — одинаковый
  `BatchResult` (детерминированный порядок обработки заявок).
- **Diagnostics обязательны**: `residual_norm`, `solve_time_ms`,
  `num_active_orders`, `config_version` в каждом результате.
- **Conservation / границы**: fill не превышает `remaining_qty`; цена fill в
  диапазоне `[price_low, price_high]` заявки (кроме явно описанных
  exceptional policies); учитываются speed caps.
- **Traceability**: `batch_id` связывает fills, order updates, risk events,
  agent logs.
- **Fallback**: ошибка solver даёт диагностируемый результат, а не молчаливое
  повреждение состояния.
- **Algorithm ↔ runtime отделены**: solver-алгоритм отделён от Kafka-runtime
  (для replay и unit-тестов).

Реальный solver заменяет симулятор **в рамках этого же контракта** — это не
отдельное решение, а реализация.

## Альтернативы

- **Непрерывный (не батчевый) matching** — отклонено для MVP: батч даёт естественную точку freeze/replay и детерминизм.
- **Без требования детерминизма** — отклонено: ломает F-15 replay parity ([ADR-028](ADR-028-backtest-replay-parity.md)).

## Последствия

- **Плюс:** замена симулятора реальным solver не ломает replay/ledger, пока соблюдён контракт.
- **Минус:** детерминизм ограничивает выбор алгоритмов (нужен стабильный порядок, фиксированные tolerance/epsilon).

## Обратимость

Контракт `BatchResult` — низкая обратимость (нужен ADR на breaking change, CLAUDE.md §15). Выбор алгоритма solver — обратим, пока соблюдён контракт и replay parity.
