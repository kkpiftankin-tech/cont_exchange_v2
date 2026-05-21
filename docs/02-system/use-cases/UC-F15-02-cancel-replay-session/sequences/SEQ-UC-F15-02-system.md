# SEQ-UC-F15-02-system. Cancel Replay Session: system view

## Type

System Context Sequence

## Feature

- [F-15](../../../features/F-15-backtest-replay/)

## Use Case

- [UC-F15-02](../use-case.md)

## Diagram

```mermaid
sequenceDiagram
    actor A as Analyst
    participant S as Continuous Exchange System

    A->>S: DELETE /api/v1/replay/sessions/{id}
    S->>S: RBAC + ownership check
    alt session.status == pending
        S->>S: status = cancelled (immediate)
    else session.status == running
        S->>S: set cancellation token; finish current batch
        S->>S: save partial AgentLog + partial Summary
        S->>S: status = cancelled
    else terminal status
        S-->>A: 409 Conflict
    end
    S-->>A: 200 OK (final ReplaySession DTO)
    S-->>A: WebSocket ReplayCancelledEvent
```

## Related Service Sequence

- [SEQ-F15-03-cancel-services](../../../../05-components/sequences/SEQ-F15-03-cancel-services.md)
