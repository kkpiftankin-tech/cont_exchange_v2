# SEQ-F12-UC-F12-01-services. Execution Hedge: service view

## Type

Service Interaction Sequence

## Feature

- [F-12](../../02-system/features/F-12-execution-hedge/)

## Use Case

- [UC-F12-01](../../02-system/use-cases/UC-F12-01-execute-hedge/use-case.md)

## Participants

- risk-manager / matching-fob-core (issuer)
- Kafka (`execution.intents`, `execution.reports`)
- external-venues
- CEX / DEX
- ledger
- observability-reporting

## Diagram

```mermaid
sequenceDiagram
    participant SRC as risk/matching
    participant K as Kafka
    participant EV as external-venues
    participant V as CEX/DEX
    participant LDG as ledger
    participant OBS as observability-reporting

    SRC->>K: publish execution.intents
    K-->>EV: consume intent
    EV->>V: place child order
    V-->>EV: fill / partial / reject
    EV->>K: publish execution.reports
    K-->>LDG: consume execution.reports → apply hedge
    K-->>OBS: log execution lifecycle
```

## Contract Binding Table

| Step | Transport | Contract | Location |
| --- | --- | --- | --- |
| SRC → Kafka | Kafka | `execution.intents` (ExecutionIntent) | [../../06-api/messaging/execution-intents.md](../../06-api/messaging/execution-intents.md) |
| EV → Kafka | Kafka | `execution.reports` (ExecutionReport) | [../../06-api/messaging/execution-reports.md](../../06-api/messaging/execution-reports.md) |
| EV → V | venue SDK | venue-specific | (out of scope) |

## Data Binding Table

| Data Object | Storage | Location |
| --- | --- | --- |
| `execution_reports` | ClickHouse (planned) | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| `positions` | PostgreSQL | [../../07-data/data-overview.md](../../07-data/data-overview.md) |
| `accounts` | PostgreSQL | [../../07-data/data-overview.md](../../07-data/data-overview.md) |

## Related Components

- [external-venues](../external-venues/overview.md)
- [risk-manager](../risk-manager/overview.md)
- [matching-fob-core](../matching-fob-core/overview.md)
- [ledger](../ledger/overview.md)
- [observability-reporting](../observability-reporting/overview.md)
