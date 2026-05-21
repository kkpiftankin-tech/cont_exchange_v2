# SEQ-UC-F12-05-system. Timeout / Underfilled / Reconciliation: system view

## Type

System Context Sequence

## Feature

- [F-12](../../../features/F-12-execution-hedge/)

## Use Case

- [UC-F12-05](../use-case.md)

## Participants

- Continuous Exchange System (Adapter + Reconciliation watcher)
- External Venue (с open child orders)
- Operator (наблюдатель risk alert)

## Diagram

```mermaid
sequenceDiagram
    participant S as Continuous Exchange System
    participant V as External Venue
    actor O as Operator

    Note over S: hedgeTimeoutMs истёк, есть открытые child orders
    S->>V: CancelOrder (all open child orders)
    V-->>S: cancel ack
    S->>S: Compute gap = targetQty - filledQty
    alt gap ≤ reconciliationGapThreshold
        S->>S: status = COMPLETED
    else gap > threshold
        S->>S: status = UNDERFILLED
        S-->>O: risk.alert(HEDGE_UNDERFILL, hedge_flow_id, gap)
        opt auto_retry_on_underfill
            S->>S: Create retry ExecutionIntent (urgency=HIGH)
        end
    end
```

## Related Service Sequence

- [SEQ-F12-03-error-scenarios-services](../../../../05-components/sequences/SEQ-F12-03-error-scenarios-services.md)

## Source Fragments

- IN-005 §4 «Error scenarios»
- IN-005 §5 «Reconciliation»
