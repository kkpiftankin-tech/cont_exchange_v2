<!--
---
id: UC-F05A-05
title: "Replay Vector Clearing Scenario"
level: sea
parent-feature: F-05A
system-sequence: "sequences/SEQ-UC-F05A-05-system.md"
service-sequence: "../../../05-components/sequences/SEQ-F05A-UC-F05A-05-services.md"
---
-->

# UC-F05A-05. Replay Vector Clearing Scenario

## Feature

- [F-05A. Vectorized External Liquidity](../../features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Primary Actor

Backtest / Replay Service (`backtest`)

## Supporting Actors

- Operator / Researcher, market-data (vectorization), matching (`vector_qp_solver`), ClickHouse

## Preconditions

- Captured external order book snapshots / `VectorClearingInput` доступны в replay-корпусе.
- Solver в детерминированном режиме ([ADR-045](../../../03-architecture/adr/ADR-045-qp-solver-backend.md): fixed max_iter, no adaptive-rho).
- ⚠️ Требует grouped/vector пути в `cpp/backtest` (тот же незакрытый разрыв AC-F09-010, CN-IN014-05).

## Trigger

Оператор/исследователь создаёт replay-сессию по captured сценарию.

## Main Flow

1. Backtest загружает captured snapshots / `VectorClearingInput` (shadow namespace).
2. Vectorization воспроизводится (тот же `W`).
3. Solver воспроизводит QP → `x`, `π`, `residual`.
4. Система сравнивает `(W, x, π, residual, ExecutionGroup)` с оригиналом — должны совпадать **бит-в-бит** (AC-F05A-011).
5. Результаты replay сохраняются (`grouped_replay_results` / vector-diagnostics) для сравнения.

## Alternative Flows

### A1. Недетерминизм обнаружен

1. `(W,x,residual)` расходится с оригиналом → replay помечает non-deterministic, эмитится diagnostic (сигнал о нарушении ADR-045 solver-настроек).

## Postconditions

- Подтверждён детерминизм vector clearing на исторических данных (либо зафиксировано расхождение).

## Related Sequence Diagrams

- System sequence: [sequences/SEQ-UC-F05A-05-system.md](sequences/SEQ-UC-F05A-05-system.md)
- Service sequence: [../../../05-components/sequences/SEQ-F05A-UC-F05A-05-services.md](../../../05-components/sequences/SEQ-F05A-UC-F05A-05-services.md)

## Related Contracts

- `backtest.execution.groups` (replay-isolated); `contracts/proto/fob/marketdata/v1/vector_liquidity.proto` (planned)

## Related Components

- `backtest`, `matching`, `market-data`

## Related Data

- CH `vector_clearing_results`, `grouped_replay_results`

## Related ADR

- [ADR-045 QP solver backend (determinism)](../../../03-architecture/adr/ADR-045-qp-solver-backend.md)
