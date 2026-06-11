<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-F15-02-replay-cycle-services
level: sea
---
-->

# SEQ-F15-02-replay-cycle-services. Replay: per-batch cycle — service view

## Type

Service Interaction Sequence

## Feature

- [F-15](../../02-system/features/F-15-backtest-replay/)

## Use Case

- [UC-F15-01 main flow](../../02-system/use-cases/UC-F15-01-create-replay-session/use-case.md)

## Purpose

Главный replay-цикл: backtest-service оркестрирует загрузку истории,
инициализацию shadow ledger, и per-batch вызовы matching/risk/ledger в
isolation mode, запись AgentLog в ClickHouse и summary в PostgreSQL.

## Participants

- backtest-service (`RunReplaySessionUC`)
- postgres (`replay_sessions`, `replay_summaries`)
- clickhouse (исторические `batchresults`, `fills`, `marketdata_snapshots`; целевая `replay_agentlogs`)
- matching-fob-core (Solver isolation gRPC)
- risk-manager (RiskService.CheckPostTrade)
- settlement-ledger (in-memory shadow namespace, reuse `ledger_uc.cpp`)
- kafka (`replay.results`)

## Diagram

```mermaid
sequenceDiagram
    participant BS as backtest-service
    participant PG as postgres
    participant CH as clickhouse
    participant MB as matching (Solver isolation)
    participant RM as risk-manager
    participant CL as shadow-ledger (in-memory)
    participant K as kafka (replay.results)

    BS->>PG: SELECT replay_sessions WHERE status='pending' (worker poll)
    PG-->>BS: pending session
    BS->>PG: UPDATE replay_sessions SET status='running', started_at=now()
    BS->>CL: Init shadow ledger namespace(session_id, initial_balances)

    BS->>CH: SELECT batchresults, fills, marketdata_snapshots WHERE time BETWEEN from AND to ORDER BY time
    CH-->>BS: N batches stream

    loop For each batch i = 1..N
        Note over BS: Check cancellation flag; break if cancelled [L2 detail]
        BS->>BS: RestoreState (PnL, positions, prev reward)
        BS->>BS: Build ReplayBatchInput (strategy FlowOrders + historical orders + refprice)
        BS->>MB: gRPC Solver/Solve(BatchRequest)
        MB-->>BS: BatchResult + FillEvent[]
        BS->>RM: gRPC RiskService/CheckPostTrade(fills, positions, risk_limits)
        RM-->>BS: ok | alert
        BS->>CL: ApplyFills(shadow namespace, fills, fee_model)
        CL-->>BS: shadow positions + incremental PnL
        BS->>BS: ComputeReward(mode, pnl, is)
        BS->>CH: INSERT replay_agentlogs (state/action/reward + diagnostics)
        BS->>K: produce ReplayProgressEvent (batch_seq, status=running)
    end

    BS->>BS: AggregateSummary (Sharpe, IS, VWAP, FillRate, MaxDD, ...)
    BS->>PG: INSERT replay_summaries
    BS->>PG: UPDATE replay_sessions SET status='completed', completed_at=now()
    BS->>K: produce ReplayCompletedEvent
```

## Contract Binding Table

| Step | Transport | Contract | Location |
| --- | --- | --- | --- |
| BS ↔ PG (session lifecycle) | SQL | `replay_sessions`, `replay_summaries` DDL | [../../07-data/replay-sessions.md](../../07-data/replay-sessions.md), [../../07-data/replay-summaries.md](../../07-data/replay-summaries.md) |
| BS ↔ CH (history read) | HTTP/SQL | `batchresults`, `fills`, `marketdata_snapshots` | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| BS ↔ CH (write replay_agentlogs) | HTTP `JSONEachRow` | ClickHouse INSERT | [../../07-data/replay-agentlogs.md](../../07-data/replay-agentlogs.md) |
| BS → MB | gRPC | `fob.matching.v1.Solver/Solve` | [contracts/proto/fob/matching/v1/solver.proto](../../../contracts/proto/fob/matching/v1/solver.proto) |
| BS → RM | gRPC | `fob.risk.v1.RiskService/CheckPostTrade` | [contracts/proto/fob/risk/v1/risk.proto](../../../contracts/proto/fob/risk/v1/risk.proto) |
| BS ↔ CL (in-process) | C++ | `IShadowLedger`, reuse `cpp/ledger/src/app/ledger_uc.cpp` | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| BS → K | Kafka | `replay.results` (Progress / Completed / Failed) | [../../06-api/messaging/replay-topics.md](../../06-api/messaging/replay-topics.md) |

## Data Binding Table

| Data Object | Storage | Location |
| --- | --- | --- |
| `replay_agentlogs` | ClickHouse | [../../07-data/replay-agentlogs.md](../../07-data/replay-agentlogs.md) |
| `replay_summaries` | PostgreSQL | [../../07-data/replay-summaries.md](../../07-data/replay-summaries.md) |
| `replay_sessions` | PostgreSQL | [../../07-data/replay-sessions.md](../../07-data/replay-sessions.md) |
| `batchresults` (read) | ClickHouse | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| `fills` (read) | ClickHouse | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| `marketdata_snapshots` (read) | ClickHouse | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| shadow positions | in-memory | n/a (lifecycle = session) |

## Failure Modes

- **Soft batch failure** (residual_norm > tolerance, solver не сошёлся, отсутствие fills): AgentLog с `solver_error_flag=1`, `failure_component`, replay продолжается. F15-35..F15-38.
- **Hard session failure** (ClickHouse down, Postgres недоступен, crash): `UPDATE status='failed'`, `error_details` заполнен, partial summary сохраняется. F15-39.
- **Cancel mid-flight**: `CancellationToken.IsCancelled()=true` после текущего батча, partial summary сохраняется. См. [SEQ-F15-03](SEQ-F15-03-cancel-services.md).

## Related Components

- [backtest-service](../backtest-service/overview.md)
- [matching-fob-core](../matching-fob-core/overview.md)
- [risk-manager](../risk-manager/overview.md)
- [ledger](../ledger/overview.md)
