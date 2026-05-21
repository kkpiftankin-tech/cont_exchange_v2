# SEQ-F11-05-execute-on-venue-services. Execute on venue: service view

## Type

Service Interaction Sequence

## Feature

- [F-11](../../02-system/features/F-11-external-venues-lob-to-fob/) (adapter уровня)
- [F-12](../../02-system/features/F-12-execution-hedge/) (business logic)

## Use Case

- [UC-F11-05](../../02-system/use-cases/UC-F11-05-execute-hedge-on-venue/use-case.md)
- [UC-F12-01](../../02-system/use-cases/UC-F12-01-execute-hedge/use-case.md) (cross-link)

## Purpose

Cross-component поток исполнения child-orders на внешней площадке. Source — Execution Planning (F-12), sink — Ledger через `execution.venue`.

## Participants

- Execution Planning (F-12) — producer `execution.intents`
- Kafka `execution.intents`
- ExecutionIntentsConsumer (cpp/venues)
- ExecuteOnVenue use case (cpp/venues — app)
- Venue Adapter (cpp/venues — infra)
- External Venue (CEX / DEX / AMM)
- ExecutionReportProducer (cpp/venues — infra)
- Kafka `execution.venue` / `execution.reports`
- Ledger (consumer)
- CircuitBreaker check (cpp/venues runtime via venue health cache)

## Diagram

```mermaid
sequenceDiagram
    participant EP as Execution Planning (F-12)
    participant K as Kafka
    participant CONS as ExecutionIntentsConsumer
    participant UC as ExecuteOnVenue
    participant ADAPT as Venue Adapter
    participant V as External Venue
    participant REP as ExecutionReportProducer
    participant LDG as ledger

    EP->>K: produce execution.intents (ExecutionIntent)
    K-->>CONS: consume execution.intents (group=venues)
    CONS->>UC: execute(intent)

    UC->>UC: check CB / routing_recommendation
    alt CB OPEN or routing=BLOCK
        UC->>REP: ExecutionReport status=REJECTED
        REP->>K: produce execution.venue
    else allow
        UC->>ADAPT: place child order (REST/WS/RPC)
        ADAPT->>V: send order
        alt fill / partial
            V-->>ADAPT: fill event
            ADAPT-->>UC: fill
            UC->>REP: ExecutionReport status=FILLED/PARTIAL_FILL
            REP->>K: produce execution.venue + legacy execution.reports
        else reject / timeout
            V--xADAPT: reject / timeout
            UC->>REP: ExecutionReport status=REJECTED/TIMEOUT
            REP->>K: produce execution.venue
        end
    end

    K-->>LDG: consume execution.venue
    LDG->>LDG: apply to hedge balance (F-12 idempotent by intent_id)
```

## Contract Binding Table

| Step                            | Transport | Contract                                                                                          | Location                                                                                                              |
| ------------------------------- | --------- | ------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| EP → Kafka                      | Kafka     | `execution.intents`, `fob.execution.v1.ExecutionIntent`                                            | [docs/06-api/messaging/execution-intents.md](../../06-api/messaging/execution-intents.md)                             |
| CONS ← Kafka                    | Kafka     | same                                                                                              | [cpp/venues/src/infra/execution_intents_consumer.cpp](../../../cpp/venues/src/infra/execution_intents_consumer.cpp)   |
| Adapter → Venue                 | venue SDK | venue-native                                                                                      | venue docs                                                                                                            |
| REP → Kafka                     | Kafka     | `execution.venue`, `fob.execution.v1.ExecutionReport`                                              | [docs/06-api/messaging/venue-topics.md#execution-venue](../../06-api/messaging/venue-topics.md#execution-venue)       |
| REP → Kafka (legacy)            | Kafka     | `execution.reports`                                                                                | [docs/06-api/messaging/execution-reports.md](../../06-api/messaging/execution-reports.md)                             |
| Ledger ← Kafka                  | Kafka     | `execution.venue`; idempotent by `intent_id`                                                       | [docs/06-api/grpc/ledger-apply-execution-report.md](../../06-api/grpc/ledger-apply-execution-report.md)               |

## Data Binding Table

| Data Object                  | Storage     | Notes                                                                  |
| ---------------------------- | ----------- | ---------------------------------------------------------------------- |
| `execution.venue` history    | Kafka       | retention 7 days (см. `infra/kafka/create_topics.sh`)                  |
| hedge balance                | PostgreSQL  | внутри ledger; F-12 owns                                               |
| backtest.execution.venue     | Kafka       | используется только при backtest session                               |

## Related Components

- [venue-execution-adapter](../venue-execution-adapter/overview.md)
- [external-venues-connector](../external-venues-connector/overview.md)
- [ledger](../ledger/overview.md)
