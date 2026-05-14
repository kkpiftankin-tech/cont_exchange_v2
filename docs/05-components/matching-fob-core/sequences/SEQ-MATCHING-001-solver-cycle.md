# SEQ-MATCHING-001-solver-cycle. Внутренний цикл solver Matching Backend

## Type

Internal Component Sequence (matching-fob-core).

## Feature

- [F-04 Batch Clearing](../../../02-system/features/F-04-batch-clearing/)

## Use Case

- [UC-F04-01](../../../02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md)

## Purpose

Показать внутренние шаги одного цикла batch-клиринга в Matching Backend: от срабатывания Scheduler-таймера до публикации в Kafka. Scheduler и solver — это не отдельные сервисы, а **части одного контейнера** `matching-fob-core` (см. [`docs/03-architecture/container-diagram.md`](../../../03-architecture/container-diagram.md)).

## Participants (внутри одного контейнера)

- **Scheduler** — внутренний таймер, срабатывает каждые `solver_config.batch_interval_ms`.
- **RunBatch orchestrator** — координирует чтение, расчёт, запись.
- **LoadActiveOrdersRepository** — DAO для `flow_orders`.
- **MarketDataClient** — gRPC-клиент к `market-data` (внешний компонент).
- **Solver** — численный алгоритм клиринга.
- **BatchPublisher** — Kafka producer для `batch.outputs` (и planned `fills`).
- **LedgerClient** — gRPC-клиент для записи `BatchResult` / fills (внешний компонент).

## Diagram

```mermaid
sequenceDiagram
    participant TIMER as Scheduler<br/>(internal timer)
    participant ORCH as RunBatch orchestrator
    participant REPO as LoadActiveOrders<br/>Repository
    participant MDC as MarketDataClient
    participant SOLVER as Solver
    participant PUB as BatchPublisher
    participant LDGC as LedgerClient

    Note over TIMER: tick каждые batch_interval_ms
    TIMER->>ORCH: RunBatch(batch_id, ts_batch)

    ORCH->>REPO: loadActiveFlowOrders(ts_batch)
    REPO-->>ORCH: FlowOrder[] (status=active, окно содержит ts_batch)

    ORCH->>ORCH: assets = collectAssetsFrom(orders)
    ORCH->>MDC: getReferencePrices(assets, ts_batch)
    MDC-->>ORCH: map<asset, ReferencePrice>

    ORCH->>SOLVER: solveBatch(batch_id, orders, refPrices, config)
    activate SOLVER
    Note over SOLVER: 1. Построить D_i(w_i) по CSLO<br/>2. Решить D(p)=0 при x_i ∈ [0, q_i]<br/>3. Вычислить clear_prices и executed_rates<br/>4. Per-order: exec_qty, exec_price, fees, liquidity_source<br/>5. Посчитать residual_norm, solve_time_ms<br/>6. Собрать BatchResult + FillEvent[]
    SOLVER-->>ORCH: { batchResult, fillEvents[] }
    deactivate SOLVER

    alt residual_norm > tolerance
        ORCH->>ORCH: пометить batchResult флагом stopped_by_max_iterations / degraded
    end
    alt solve_time_ms > SLA
        ORCH->>ORCH: пометить batchResult признаком SLA-breach
    end

    ORCH->>LDGC: saveBatchResult(batchResult) / saveFills(fillEvents)
    LDGC-->>ORCH: ok

    ORCH->>PUB: publish batch.outputs (BatchResult + fills)
    Note right of PUB: см. Conflict Note C-1:<br/>спецификация IN-003 разделяет на<br/>`batch.outputs` и `fills`; MVP использует<br/>единый topic
    PUB-->>ORCH: ack
```

## Внутренний контракт `RunBatch(batch_id, ts_batch)`

```text
1. orders ← LoadActiveOrdersRepository.loadActiveFlowOrders(ts_batch)
2. assets ← collectAssetsFrom(orders)
3. refPrices ← MarketDataClient.getReferencePrices(assets, ts_batch)
4. solution ← Solver.solveBatch(batch_id, orders, refPrices, config)
5. LedgerClient.saveBatchResult(solution.batchResult)
6. LedgerClient.saveFills(solution.fillEvents)
7. BatchPublisher.publish("batch.outputs", solution.batchResult)
   (planned) BatchPublisher.publish("fills", solution.fillEvents)
```

## Внутренний контракт `solveBatch(batch_id, orders, refPrices, config) → { batchResult, fillEvents[] }`

См. [F-04-batch-clearing/README.md](../../../02-system/features/F-04-batch-clearing/README.md) §«Алгоритм».

Этапы:

1. Построение функций спроса/предложения `D_i(w_i)` по каждой FlowOrder на основе её CSLO-параметров (`p_low`, `p_high`, `q_rate`, `Q_max`).
2. Решение системы `D(p) = 0` по вектору цен/скоростей с ограничениями `x_i ∈ [0, q_i]`.
3. Вычисление `clear_prices[asset]` и `executed_rates[asset]`.
4. Распределение исполнений по FlowOrder: `exec_qty`, `exec_price`, `fees`, `liquidity_source`.
5. Подсчёт `residual_norm`, `solve_time_ms`, `solver_diagnostics`.
6. Формирование `BatchResult` и `FillEvent[]`.

Инварианты solver (см. также [test plan](../../../10-testing/features/F-04-test-plan.md)):

- `min(p_L,i) ≤ clear_price[asset] ≤ max(p_H,i)` для каждого инструмента.
- Сумма buy по активу = сумме sell (внутренний баланс) с точностью до `residual_norm`.
- `exec_qty ≤ remaining_qty` каждой заявки.
- Идентичный вход → идентичный `BatchResult` (детерминизм для replay).

## Граничные случаи

| Случай | Что меняется в потоке | Куда идут данные |
| --- | --- | --- |
| Нет активных заявок | `orders == []`, `solveBatch` возвращает `BatchResult` с пустыми `executed_rates` и без FillEvent | Публикуется пустой `batch.outputs`; Observability фиксирует метрику пустого батча |
| Solver не сошёлся (`residual_norm > tolerance` или достигнут `max_iterations`) | Orchestrator помечает `BatchResult` флагом `degraded`; срабатывает alert; опционально fallback на epsilon-MM или остановка | Дополнительно `risk.alerts(SOLVER_NOT_CONVERGED)` |
| Нарушен SLA `solve_time_ms` | Orchestrator помечает `BatchResult` признаком `sla_breach` | `risk.alerts(SLA_BREACH)` + потенциально kill-switch (F-16) |

## Contract Binding Table

| Step | Transport | Contract | Location |
| --- | --- | --- | --- |
| REPO → PG | SQL | `SELECT * FROM flow_orders WHERE status='active' AND window covers ts_batch` | [docs/07-data/oltp-schema.md#таблица-flow_orders](../../../07-data/oltp-schema.md#таблица-flow_orders) |
| ORCH → MDC | gRPC | `fob.marketdata.v1.MarketDataService/GetReferencePrices` (planned) | [docs/06-api/grpc/marketdata-get-reference-prices.md](../../../06-api/grpc/marketdata-get-reference-prices.md) |
| ORCH → LDGC | gRPC | `fob.ledger.v1.LedgerService/ApplyBatchResult` (idempotent by `batch_id+order_id`) | [docs/06-api/grpc/ledger-apply-batch-result.md](../../../06-api/grpc/ledger-apply-batch-result.md) |
| PUB → Kafka | Kafka | `batch.outputs` (BatchResult) — текущая схема; planned split → `batch.outputs` + `fills` (см. C-1) | [docs/06-api/messaging/batch-outputs.md](../../../06-api/messaging/batch-outputs.md) |

## Data Binding Table

| Data Object | Storage | Notes |
| --- | --- | --- |
| `flow_orders` | PostgreSQL | source of truth для активных заявок |
| `solver_config` (R) | PostgreSQL | active row; кэш в Matching Backend, hot-reload |
| `fills` | ClickHouse | через Kafka ingestion |
| `batch_results` | ClickHouse | через Kafka ingestion |

## Related

- Feature: [F-04](../../../02-system/features/F-04-batch-clearing/)
- Service sequence (cross-component): [SEQ-F04-UC-F04-01-services](../../sequences/SEQ-F04-UC-F04-01-services.md)
- System sequence (black-box): [SEQ-UC-F04-01-system](../../../02-system/use-cases/UC-F04-01-run-batch-clearing/sequences/SEQ-UC-F04-01-system.md)
- Test plan: [F-04 test plan](../../../10-testing/features/F-04-test-plan.md)

## Source Fragments

- IN-003-FR-017 (Scheduler → MB → MDS → Kafka)
- IN-003-FR-019 (данные I/O)
- IN-003-FR-020 (алгоритм)
- IN-003-FR-021 (методы RunBatch / solveBatch)
