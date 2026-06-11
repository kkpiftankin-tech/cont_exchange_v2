<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-F15-04-audit-mode-services
level: sea
---
-->

# SEQ-F15-04-audit-mode-services. Replay: audit-mode — service view

## Type

Service Interaction Sequence

## Feature

- [F-15](../../02-system/features/F-15-backtest-replay/)

## Use Case

- [UC-F15-04](../../02-system/use-cases/UC-F15-04-audit-mode-replay/use-case.md)

## Participants

- gateway
- backtest-service (`RunReplayAuditBatchUC`)
- clickhouse (`batchresults`, `fills`, `marketdata_snapshots`)
- postgres (`solver_config`, `risk_limits`, `replay_audit_runs`)
- matching-fob-core (Solver isolation)

## Diagram

```mermaid
sequenceDiagram
    actor U as Auditor
    participant GW as gateway
    participant BS as backtest-service (Audit UC)
    participant CH as clickhouse
    participant PG as postgres
    participant MB as matching (Solver isolation)

    U->>GW: POST /api/v1/replay/audit-runs (batch_id, tolerance, override?)
    GW->>BS: HTTP forward
    BS->>BS: RbacEngine.Authorize(replay:execute + admin)
    BS->>PG: INSERT replay_audit_runs (status=pending)

    BS->>CH: SELECT * FROM batchresults WHERE batch_id=?
    CH-->>BS: production BatchResult
    BS->>CH: SELECT flow_orders snapshot + marketdata_snapshot at ts
    CH-->>BS: inputs
    BS->>PG: SELECT solver_config WHERE version = batchresults.config_version
    PG-->>BS: snapshot configs

    BS->>MB: gRPC Solver/Solve(reconstructed inputs)
    MB-->>BS: replay BatchResult + fills
    BS->>BS: Diff(production, replay) → diff_json
    BS->>BS: IsEquivalent(diff, tolerance) → bool

    BS->>PG: UPDATE replay_audit_runs SET status='completed', equivalent=?, diff_json=?, replay_result_json=?, completed_at=now()
    BS-->>GW: 200 OK {audit_run_id, equivalent, diff}
    GW-->>U: 200 OK
```

## Contract Binding Table

| Step | Transport | Contract | Location |
| --- | --- | --- | --- |
| User → GW | REST | `POST /api/v1/replay/audit-runs` | [../../06-api/rest/replay.md](../../06-api/rest/replay.md) |
| BS → CH | SQL | `SELECT batchresults`, `SELECT marketdata_snapshots` | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| BS → PG (config) | SQL | `solver_config @version`, `risk_limits @version` | [../../07-data/oltp-schema.md](../../07-data/oltp-schema.md) |
| BS → MB | gRPC | `fob.matching.v1.Solver/Solve` | [contracts/proto/fob/matching/v1/solver.proto](../../../contracts/proto/fob/matching/v1/solver.proto) |
| BS → PG (audit row) | SQL | `INSERT/UPDATE replay_audit_runs` | [../../07-data/replay-sessions.md](../../07-data/replay-sessions.md) |

## Data Binding Table

| Data Object | Storage | Location |
| --- | --- | --- |
| `replay_audit_runs` | PostgreSQL | [../../07-data/replay-sessions.md](../../07-data/replay-sessions.md#таблица-replay_audit_runs) |
| `batchresults` (read) | ClickHouse | [../../07-data/data-overview.md](../../07-data/data-overview.md) |

## Related Components

- [backtest-service](../backtest-service/overview.md)
- [matching-fob-core](../matching-fob-core/overview.md)
