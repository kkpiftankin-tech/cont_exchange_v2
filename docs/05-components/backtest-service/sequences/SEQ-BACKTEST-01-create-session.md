<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-BACKTEST-01-create-session
level: fish
component: backtest-service
---
-->

# SEQ-BACKTEST-01. Internal: CreateReplaySession

## Type

Internal Component Sequence (внутри `cpp/backtest`)

## Feature

- [F-15](../../../02-system/features/F-15-backtest-replay/)

## Use Case

- [UC-F15-01](../../../02-system/use-cases/UC-F15-01-create-replay-session/use-case.md)

## Purpose

Внутренняя последовательность вызовов в `cpp/backtest` при обработке
`CreateReplaySessionUC`. Service-level контекст — в
[SEQ-F15-01-create-session-services](../../sequences/SEQ-F15-01-create-session-services.md).

## Diagram

```mermaid
sequenceDiagram
    participant HTTP as HTTP handler (Crow)
    participant UC as CreateReplaySession UC
    participant RBAC as RbacEngine
    participant V as ValidateStrategy
    participant HL as HistoricalBatchLoader
    participant CSB as ReplayConfigSnapshotBuilder
    participant SR as PostgresReplaySessionRepository
    participant RR as PostgresRbacRepository
    participant EP as KafkaReplayEventPublisher

    HTTP->>UC: Run(Request{user_id, strategy, range, configs})
    UC->>RBAC: Authorize(user, "replay:create")
    alt denied
        RBAC-->>UC: AuthorizationResult{allowed=false}
        UC->>RR: WriteAuditLog(status="denied")
        UC-->>HTTP: Result{ok=false, 403}
    end
    UC->>V: Validate(strategy)
    alt invalid
        UC->>RR: WriteAuditLog(status="failure")
        UC-->>HTTP: Result{ok=false, 400, error_code=validation_error}
    end
    UC->>HL: CheckCoverage(date_range)
    alt no data
        UC->>RR: WriteAuditLog(status="failure")
        UC-->>HTTP: Result{ok=false, 400, error_code=no_historical_data}
    end
    UC->>CSB: Build(configs, fee_model, reward_config, seed)
    CSB-->>UC: snapshot_json
    UC->>SR: Insert(ReplaySession{session_id, status=pending, snapshot, ...})
    SR-->>UC: ok
    UC->>RR: WriteAuditLog(status="success", new_values=session_json)
    UC->>EP: PublishLifecycle(ReplayProgressEvent{status=pending})
    UC-->>HTTP: Result{ok=true, created=ReplaySession}
```
