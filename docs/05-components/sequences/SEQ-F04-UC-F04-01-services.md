# SEQ-F04-UC-F04-01-services. Batch Clearing: service view

## Type

Service Interaction Sequence

## Feature

- [F-04](../../02-system/features/F-04-batch-clearing/)

## Use Case

- [UC-F04-01](../../02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md)

## Purpose

Внутренний цикл клиринга. Триггер — внутренний Scheduler внутри matching-fob-core (срабатывает каждые `solver_config.batch_interval_ms`). Распространение результатов через Kafka в ledger, risk и observability.

Внутренние шаги solver (RunBatch, loadActiveFlowOrders, getReferencePrices, solveBatch, saveBatchResult, saveFills) — см. [SEQ-MATCHING-001-solver-cycle](../matching-fob-core/sequences/SEQ-MATCHING-001-solver-cycle.md).

## Participants

- matching-fob-core
- Kafka (`orders.normalized` → matching; `batch.outputs` → ledger/risk/observability)
- ledger
- risk-manager
- observability-reporting

## Diagram

```mermaid
sequenceDiagram
    participant M as matching-fob-core
    participant K as Kafka
    participant LDG as ledger
    participant RISK as risk-manager
    participant OBS as observability-reporting

    Note over M: BatchTimer fires
    K-->>M: consume orders.normalized (active state)
    M->>M: solver.run() → BatchResult
    M->>K: publish batch.outputs

    K-->>LDG: consume batch.outputs
    LDG->>LDG: apply fills (idempotent by batch_id+order_id)

    K-->>RISK: consume batch.outputs
    RISK->>RISK: post-trade check (margin, position)
    opt margin breach
        RISK->>K: publish risk.alerts
    end

    K-->>OBS: consume batch.outputs
    OBS->>OBS: persist diagnostics
```

## Contract Binding Table

| Step | Transport | Contract | Location |
| --- | --- | --- | --- |
| M consumes | Kafka | `orders.normalized` | [../../06-api/messaging/orders-normalized.md](../../06-api/messaging/orders-normalized.md) |
| M produces | Kafka | `batch.outputs` (BatchResult, Fill, OrderUpdate) | [../../06-api/messaging/batch-outputs.md](../../06-api/messaging/batch-outputs.md) |
| RISK produces | Kafka | `risk.alerts` | [../../06-api/messaging/risk-alerts.md](../../06-api/messaging/risk-alerts.md) |

## Data Binding Table

| Data Object | Storage | Location |
| --- | --- | --- |
| `fills` | ClickHouse (planned) | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| `batch_results` | ClickHouse (planned) | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| `positions` | PostgreSQL (planned) | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| `accounts` | PostgreSQL (planned) | [../../07-data/data-overview.md](../../07-data/data-overview.md) |

## Related Components

- [matching-fob-core](../matching-fob-core/overview.md)
- [ledger](../ledger/overview.md)
- [risk-manager](../risk-manager/overview.md)
- [observability-reporting](../observability-reporting/overview.md)
