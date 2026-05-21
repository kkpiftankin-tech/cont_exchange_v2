# SEQ-UC-F12-02-system. Manual Operator Hedge: system view

## Type

System Context Sequence

## Feature

- [F-12](../../../features/F-12-execution-hedge/)

## Use Case

- [UC-F12-02](../use-case.md)

## Participants

- Operator (внешний actor)
- Continuous Exchange System
- External Venue (CEX/DEX/AMM)

## Diagram

```mermaid
sequenceDiagram
    actor O as Operator
    participant S as Continuous Exchange System
    participant V as External Venue

    O->>S: POST /api/v1/hedge/intents/manual<br/>(provider_id, symbol, side, target_qty, urgency)
    S->>S: Auth + RBAC check (operator/admin)
    S->>S: Validate params
    S->>S: Create ExecutionIntent (source=MANUAL_OVERRIDE)
    S->>S: Pre-hedge risk check
    alt Risk OK
        S->>V: Place child orders
        V-->>S: Fills
        S-->>O: HTTP 201 HedgeFlow{status=OPEN/COMPLETED}
        S-->>O: Subsequent updates via UI/SSE
    else Risk REJECT
        S-->>O: HTTP 422 RISK_REJECTED + details
    else Venue unavailable
        S-->>O: HTTP 422 VENUE_UNAVAILABLE
    end
```

## Related Service Sequence

- [SEQ-F12-01-auto-hedge-services](../../../../05-components/sequences/SEQ-F12-01-auto-hedge-services.md) (после Gateway → одинаковая цепочка)

## Source Fragments

- IN-005 §9 «REST API»
- `contracts/openapi/fob/hedge/v1/api/hedge.yaml` operationId `createManualIntent`
