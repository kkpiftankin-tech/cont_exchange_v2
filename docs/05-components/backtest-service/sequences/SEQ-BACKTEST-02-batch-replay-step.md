<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-BACKTEST-02-batch-replay-step
level: fish
component: backtest-service
---
-->

# SEQ-BACKTEST-02. Internal: Per-batch replay step

## Type

Internal Component Sequence (один шаг внутри `RunReplaySessionUC::Run` loop)

## Feature

- [F-15](../../../02-system/features/F-15-backtest-replay/)

## Purpose

Детализирует, что происходит внутри одной итерации основного replay-цикла
(`for batch_seq in 1..N`). Service-level — см.
[SEQ-F15-02-replay-cycle-services](../../sequences/SEQ-F15-02-replay-cycle-services.md).

## Diagram

```mermaid
sequenceDiagram
    participant RR as RunReplaySession UC (loop)
    participant CT as CancellationToken
    participant RS as RestoreState UC
    participant RBI as ReplayBatchInput builder
    participant BE as GrpcReplayBatchExecutor
    participant SL as IShadowLedger
    participant SJ as ReplayStepJournal
    participant AL as IAgentLogWriter
    participant RM as ReplayRuntimeMetrics
    participant EP as IReplayEventPublisher

    RR->>CT: IsCancelled(session_id)?
    alt cancelled
        RR-->>RR: break loop
    end
    RR->>RS: Restore(session_id, batch_seq)
    RS-->>RR: AgentState (positions, cum_pnl, prev_reward)
    RR->>RBI: Build(strategy, historical_orders, marketdata)
    RBI-->>RR: BatchRequest
    RR->>BE: Execute(BatchRequest, snapshot_configs)
    Note over BE: gRPC: Solver/Solve → BatchResult+Fills<br/>gRPC: Risk/CheckPostTrade → ok/alert
    BE-->>RR: BatchExecutionResult{BatchResult, fills, risk_status}
    RR->>SL: ApplyFills(shadow_ns, fills, fee_model)
    SL-->>RR: updated positions + incremental_pnl
    RR->>SJ: AppendBatch(BatchExecutionResult, AgentState, reward_mode)
    SJ-->>RR: AgentLog row + running summary
    RR->>AL: WriteAgentLog(row)
    AL-->>RR: ok (or solver_error_flag=1 on soft failure)
    RR->>RM: ObserveBatch(solve_time_ms, residual_norm, latency_ms)
    RR->>EP: PublishProgress(ReplayProgressEvent{batch_seq, partial_summary})
```

## Failure handling (inside one iteration)

- **Solver error / residual_norm > tolerance** → AgentLog с
  `solver_error_flag=1`, `failure_component="matching"`, `error_code`.
  Loop **продолжается** (soft failure).
- **Risk reject** → `risk_status="rejected"`, fills не применяются, AgentLog
  с `failure_component="risk"`. Soft failure.
- **Ledger error** → `failure_component="ledger"`, soft failure.
- **ClickHouse INSERT failure** → retry с exponential backoff; при
  превышении retry budget — soft failure, продолжаем (loss tolerable).
- **PostgreSQL session update failure** → hard failure, loop aborted, status
  = `failed`.
