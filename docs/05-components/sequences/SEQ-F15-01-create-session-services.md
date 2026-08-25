<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-F15-01-create-session-services
level: sea
---
-->

# SEQ-F15-01-create-session-services. Replay: create session — service view

## Type

Service Interaction Sequence

## Feature

- [F-15](../../02-system/features/F-15-backtest-replay/)

## Use Case

- [UC-F15-01](../../02-system/use-cases/UC-F15-01-create-replay-session/use-case.md)

## Purpose

Создание ReplaySession через REST/Kafka и быстрый возврат `pending`.
Длительный replay-цикл — см. [SEQ-F15-02](SEQ-F15-02-replay-cycle-services.md).

## Participants

- gateway (REST edge)
- backtest-service (cpp/backtest)
- postgres (`replay_sessions`, RBAC tables)
- clickhouse (read-only coverage check)
- kafka (`replay.results` для progress events)

## Diagram

```mermaid
sequenceDiagram
    participant CL as Analyst Client
    participant GW as gateway
    participant BS as backtest-service
    participant PG as postgres
    participant CH as clickhouse
    participant K as kafka (replay.results)

    CL->>GW: POST /api/v1/replay/sessions (strategy, range, configs, JWT)
    GW->>BS: HTTP forward (port 8087)
    Note over BS: Authorize replay:create (RBAC) [L2 detail]
    BS->>BS: ValidateStrategy (F15-22..25)
    BS->>CH: SELECT count(*) FROM batchresults WHERE event_time_ms BETWEEN ? AND ?
    CH-->>BS: N (must be > 0)
    BS->>PG: SELECT solver_config, risk_limits BY id (or use inline_json)
    PG-->>BS: configs
    Note over BS: Build replay config snapshot (configs + fee_model + reward + seed) [L2 detail]
    BS->>PG: INSERT INTO replay_sessions (session_id, status='pending', snapshot, ...)
    PG-->>BS: ok
    BS->>PG: INSERT INTO audit_log (action='create_session', status='success', ...)
    BS->>K: produce ReplayProgressEvent (status=pending)
    BS-->>GW: 201 {session_id, status=pending, created_at}
    GW-->>CL: 201 Created
```

## Contract Binding Table

| Step | Transport | Contract | Location |
| --- | --- | --- | --- |
| Client → GW | REST | `POST /api/v1/replay/sessions` | [../../06-api/rest/replay.md](../../06-api/rest/replay.md) |
| GW → BS | HTTP | same REST schema | [contracts/openapi/fob/replay/v1/api/replay.yaml](../../../contracts/openapi/fob/replay/v1/api/replay.yaml) |
| BS → CH | SQL | `SELECT batchresults` | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| BS → PG | SQL | `INSERT replay_sessions`, `INSERT audit_log` | [../../07-data/replay-sessions.md](../../07-data/replay-sessions.md), [../../07-data/replay-rbac.md](../../07-data/replay-rbac.md) |
| BS → K | Kafka | `replay.results` (`ReplayProgressEvent`) | [../../06-api/messaging/replay-topics.md](../../06-api/messaging/replay-topics.md) |

## Data Binding Table

| Data Object | Storage | Location |
| --- | --- | --- |
| `replay_sessions` | PostgreSQL | [../../07-data/replay-sessions.md](../../07-data/replay-sessions.md) |
| `audit_log` | PostgreSQL | [../../07-data/replay-rbac.md](../../07-data/replay-rbac.md) |
| `batchresults` | ClickHouse (read-only) | [../../07-data/data-overview.md](../../07-data/data-overview.md) |

## Related Components

- [backtest-service](../backtest-service/overview.md)
- [gateway](../gateway/overview.md)
