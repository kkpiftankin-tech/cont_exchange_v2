<!--
---
id: UC-F05A-02
title: "Run Vector Clearing with F-09 Solver"
level: sea
parent-feature: F-05A
system-sequence: "sequences/SEQ-UC-F05A-02-system.md"
service-sequence: "../../../05-components/sequences/SEQ-F05A-UC-F05A-02-services.md"
---
-->

# UC-F05A-02. Run Vector Clearing with F-09 Solver

## Feature

- [F-05A. Vectorized External Liquidity](../../features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Primary Actor

Matching Backend (`multileg_vector_solver`)

## Supporting Actors

- Market Data Service (`VectorClearingInput`)
- Ledger, Risk Manager, ClickHouse, Kafka (`execution.groups`, `fills`, `batch.outputs`, `risk.alerts`)

## Preconditions

- `VectorClearingInput` (W, pH, D, q, source_map) собран (UC-F05A-01).
- QP solver backend доступен ([ADR-045](../../../03-architecture/adr/ADR-045-qp-solver-backend.md), OSQP).
- `surplus_policy` сконфигурирована ([ADR-044](../../../03-architecture/adr/ADR-044-surplus-exchange-pnl-policy.md); MVP `REJECT_IF_RESIDUAL`).

## Trigger

Batch-цикл matching получает `VectorClearingInput` из `marketdata.vectorized`.

## Main Flow

1. Matching получает `VectorClearingInput`.
2. Solver решает QP: `max_x [xᵀpH − ½xᵀDx]` s.t. `Wx = 0`, `0 ≤ x ≤ q`
   (детерминированный режим для replay).
3. Solver возвращает `(x, π, residual)` + diagnostics (solveTimeMs, iterations, status).
4. Система проверяет `residualNorm = ‖Wx‖`.
5. Для каждого `x_i > 0` формируется исполнение исходного внешнего order level
   (`executed_asset_vector = x_i · w_i`; source-trace сохранён).
6. Результат публикуется как `ExecutionGroup` (`execution.mode = multileg_vector_solver`,
   source-trace на venue levels) + `FillEvent[]` + `BatchResult`; diagnostics → ClickHouse
   (`vector_clearing_results`).
7. Ledger применяет сбалансированный `ExecutionGroup` (Wx≈0 → no phantom inventory).

## Alternative Flows

### A1. Surplus / ненулевой residual ([ADR-044](../../../03-architecture/adr/ADR-044-surplus-exchange-pnl-policy.md))

1. `residualNorm > tolerance` → применяется `surplus_policy`.
2. `REJECT_IF_RESIDUAL` (MVP): группа `degraded` / не исполняется как полностью клиринговая.
3. `EXCHANGE_PNL`: остаток проводится на house-счёт (Σuser + house = 0), идемпотентно.
4. Эмитится `SurplusEvent` (CH `surplus_events`) + `risk.alerts` выше порога.

### A2. Solver failed / вырожденная W

1. Solver возвращает `failed` → группа не исполняется; diagnostic + alert.

## Postconditions

- Опубликован `ExecutionGroup` + `FillEvent[]` (либо degraded/reject без несбалансированных проводок).
- Ledger сбалансирован (no phantom inventory, §17).
- Диагностика (W, x, π, residual, surplus) в ClickHouse.

## Related Sequence Diagrams

- System sequence: [sequences/SEQ-UC-F05A-02-system.md](sequences/SEQ-UC-F05A-02-system.md)
- Service sequence: [../../../05-components/sequences/SEQ-F05A-UC-F05A-02-services.md](../../../05-components/sequences/SEQ-F05A-UC-F05A-02-services.md)

## Related Contracts

- `contracts/proto/fob/marketdata/v1/vector_liquidity.proto` (planned); [execution.groups](../../../06-api/messaging/execution-groups.md), `fills`, `batch.outputs`

## Related Components

- `matching`, `ledger`, `risk`, `market-data`

## Related Data

- CH `vector_clearing_results`, `surplus_events` (planned)

## Related ADR

- [ADR-045 QP solver backend (OSQP)](../../../03-architecture/adr/ADR-045-qp-solver-backend.md)
- [ADR-044 surplus / EXCHANGE_PNL policy](../../../03-architecture/adr/ADR-044-surplus-exchange-pnl-policy.md)
