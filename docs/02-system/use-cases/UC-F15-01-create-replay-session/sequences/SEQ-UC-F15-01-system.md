# SEQ-UC-F15-01-system. Create Replay Session: system view

## Type

System Context Sequence

## Feature

- [F-15](../../../features/F-15-backtest-replay/)

## Use Case

- [UC-F15-01](../use-case.md)

## Purpose

Создание ReplaySession инициируется аналитиком извне; внешний интерфейс
системы — REST или Kafka command. Внутри Continuous Exchange System
объединены backtest-service, matching, risk, ledger и хранилища.

## Participants

- Analyst (внешний)
- Continuous Exchange System (черный ящик)

## Diagram

```mermaid
sequenceDiagram
    actor A as Analyst
    participant S as Continuous Exchange System

    A->>S: POST /api/v1/replay/sessions (strategy, range, configs)
    S->>S: Validate RBAC + strategy + historical coverage
    S->>S: Snapshot configs (solver, risk, fees, seed)
    S->>S: Create session (status=pending)
    S-->>A: 201 Created (session_id, status=pending, created_at)

    Note over S: worker picks pending session
    S->>S: status=running; init shadow ledger
    S->>S: loop: solve → risk → apply → log
    S-->>A: WebSocket progress (replay.results)
    S-->>A: WebSocket completed / failed
```

## Related Service Sequence

- [SEQ-F15-01-create-session-services](../../../../05-components/sequences/SEQ-F15-01-create-session-services.md)
- [SEQ-F15-02-replay-cycle-services](../../../../05-components/sequences/SEQ-F15-02-replay-cycle-services.md)
