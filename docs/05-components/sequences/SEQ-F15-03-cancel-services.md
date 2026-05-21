# SEQ-F15-03-cancel-services. Replay: cancel session — service view

## Type

Service Interaction Sequence

## Feature

- [F-15](../../02-system/features/F-15-backtest-replay/)

## Use Case

- [UC-F15-02](../../02-system/use-cases/UC-F15-02-cancel-replay-session/use-case.md)

## Participants

- gateway (REST edge)
- backtest-service (`CancelReplaySessionUC`)
- backtest-service worker (`RunReplaySessionUC` running session)
- postgres (`replay_sessions`, `replay_summaries`, `audit_log`)
- clickhouse (partial `replay_agentlogs`)
- kafka (`replay.results`)

## Diagram

```mermaid
sequenceDiagram
    actor U as User
    participant GW as gateway
    participant BS as backtest-service (Cancel UC)
    participant W as backtest worker (Run UC)
    participant PG as postgres
    participant CH as clickhouse
    participant K as kafka

    U->>GW: DELETE /api/v1/replay/sessions/{id}
    GW->>BS: HTTP forward
    BS->>BS: RbacEngine.Authorize(replay:cancel)
    BS->>PG: SELECT session
    PG-->>BS: session DTO
    alt status == pending
        BS->>PG: UPDATE status='cancelled', completed_at=now()
        BS->>PG: INSERT audit_log
        BS->>K: produce ReplayCancelledEvent
        BS-->>GW: 200 OK
    else status == running
        BS->>BS: SetCancelled(session_id) (ICancellationToken)
        BS-->>GW: 200 OK (final state pending)
        Note over W: W is mid-batch
        W->>W: finish current batch
        W->>CH: INSERT replay_agentlogs (last batch)
        W->>W: AggregateSummary (partial=true)
        W->>PG: INSERT replay_summaries (partial=true)
        W->>PG: UPDATE status='cancelled', completed_at=now()
        W->>K: produce ReplayCancelledEvent (cancelled_at_batch_seq)
    else status terminal (completed/failed/cancelled)
        BS-->>GW: 409 Conflict
    end
```

## Contract Binding Table

| Step | Transport | Contract | Location |
| --- | --- | --- | --- |
| User → GW | REST | `DELETE /api/v1/replay/sessions/{id}` | [../../06-api/rest/replay.md](../../06-api/rest/replay.md) |
| BS ↔ PG | SQL | `UPDATE replay_sessions`, `INSERT audit_log` | [../../07-data/replay-sessions.md](../../07-data/replay-sessions.md), [../../07-data/replay-rbac.md](../../07-data/replay-rbac.md) |
| BS internal token | C++ | `ICancellationToken` | [cpp/backtest/src/app/cancel_replay_session_uc.hpp](../../../cpp/backtest/src/app/cancel_replay_session_uc.hpp) |
| Worker → CH | HTTP | `INSERT replay_agentlogs` | [../../07-data/replay-agentlogs.md](../../07-data/replay-agentlogs.md) |
| Worker → K | Kafka | `replay.results` (`ReplayCancelledEvent`) | [../../06-api/messaging/replay-topics.md](../../06-api/messaging/replay-topics.md) |

## Data Binding Table

| Data Object | Storage | Location |
| --- | --- | --- |
| `replay_sessions` | PostgreSQL | [../../07-data/replay-sessions.md](../../07-data/replay-sessions.md) |
| `replay_summaries` (partial=true) | PostgreSQL | [../../07-data/replay-summaries.md](../../07-data/replay-summaries.md) |
| `replay_agentlogs` (partial) | ClickHouse | [../../07-data/replay-agentlogs.md](../../07-data/replay-agentlogs.md) |
| `audit_log` | PostgreSQL | [../../07-data/replay-rbac.md](../../07-data/replay-rbac.md) |

## Related Components

- [backtest-service](../backtest-service/overview.md)
